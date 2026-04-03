#include "core_build_info.h"
#include "core_internal.h"
#include "core_runtime_constants.h"

#include <cstring>

#include <NimBLEDevice.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace core {

namespace {

static const char* TAG = "core_ble";
// BLE UUIDs and name are part of the mobile application contract.
// Contributors must treat these as externally versioned API values.
NimBLECharacteristic* g_tx = nullptr;
QueueHandle_t g_ble_rx_queue = nullptr;
StaticQueue_t g_ble_rx_queue_struct{};
uint8_t g_ble_rx_queue_storage[kBleQueueDepth * sizeof(char*)] = {};
TaskHandle_t g_ble_rx_task_handle = nullptr;

// BLE writes arrive in NimBLE host context.
// The queue hops payload processing into a normal FreeRTOS task so JSON parsing,
// HTTP calls, and MQTT-triggering side effects never run inside the BLE callback.
void ensure_ble_rx_queue() {
    if (g_ble_rx_queue) {
        return;
    }
    g_ble_rx_queue = xQueueCreateStatic(static_cast<UBaseType_t>(kBleQueueDepth),
                                        sizeof(char*),
                                        g_ble_rx_queue_storage,
                                        &g_ble_rx_queue_struct);
}

// Implements the old mobile flow where GET_TOKEN waits for the HTTP exchange
// instead of responding immediately and forcing the app to poll.
bool wait_for_device_code(RuntimeConfig& cfg) {
    const int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t last_attempt_ms = 0;

    while ((esp_timer_get_time() / 1000) - start_ms < kProvisioningTimeoutMs) {
        {
            RuntimeState& s = state();
            std::lock_guard<std::mutex> lock(s.config_mutex);
            cfg = s.config;
        }
        if (!cfg.user_code.empty() && !cfg.verification_url.empty()) {
            return true;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (last_attempt_ms == 0 || (now_ms - last_attempt_ms) >= kProvisioningRetryIntervalMs) {
            perform_device_code_exchange();
            last_attempt_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(kBleCommandPollDelayMs));
    }

    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);
    cfg = s.config;
    return !cfg.user_code.empty() && !cfg.verification_url.empty();
}

// Implements the old mobile flow where CHECK_AUTH waits for token completion
// within a bounded timeout window.
bool wait_for_access_token() {
    const int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t last_attempt_ms = 0;

    while ((esp_timer_get_time() / 1000) - start_ms < kProvisioningTimeoutMs) {
        RuntimeConfig cfg{};
        {
            RuntimeState& s = state();
            std::lock_guard<std::mutex> lock(s.config_mutex);
            cfg = s.config;
        }
        if (!cfg.access_token.empty()) {
            return true;
        }

        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (!cfg.device_code.empty() && !cfg.token_url.empty() &&
            (last_attempt_ms == 0 || (now_ms - last_attempt_ms) >= kProvisioningRetryIntervalMs)) {
            perform_token_exchange();
            last_attempt_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(kBleTokenPollDelayMs));
    }

    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);
    return !s.config.access_token.empty();
}

// Common BLE notify formatter used by every command response.
void send_ble_response(cJSON* root) {
    if (!g_tx || !root) {
        return;
    }
    char* encoded = cJSON_PrintUnformatted(root);
    if (!encoded) {
        return;
    }
    g_tx->setValue(reinterpret_cast<const uint8_t*>(encoded), static_cast<size_t>(std::strlen(encoded)));
    g_tx->notify();
    ESP_LOGI(TAG, "Sent BLE notify (%u bytes): %s",
             static_cast<unsigned>(std::strlen(encoded)),
             encoded);
    free(encoded);
}

// Backward-compatible path for the oldest BLE payload shape: `ssid:password`.
void handle_wifi_legacy_payload(const std::string& payload) {
    const size_t split = payload.find(':');
    if (split == std::string::npos || split == 0 || split + 1 >= payload.size()) {
        return;
    }

    RuntimeState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        s.config.wifi_ssid = payload.substr(0, split);
        s.config.wifi_password = payload.substr(split + 1);
    }
    save_runtime_config();
    connect_wifi_if_configured();

    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "command", "SERVER_DATA");
    cJSON_AddBoolToObject(response, "isSuccess", s.wifi_connected.load());
    cJSON_AddStringToObject(response, "error", s.wifi_connected.load() ? "" : "Failed to connect to WiFi");
    cJSON_AddNumberToObject(response, "byteReceived", static_cast<double>(payload.size()));
    send_ble_response(response);
    cJSON_Delete(response);
}

// Main BLE provisioning command handler.
//
// Supported commands:
// - `SERVER_DATA`: stores remote URLs and cloud endpoints
// - `GET_TOKEN`: performs device-code exchange and returns user-facing auth data
// - `CHECK_AUTH`: waits for access token issuance and then reboots into runtime
void handle_json_payload(const std::string& payload) {
    ESP_LOGI(TAG, "Received BLE value (%u bytes): %s",
             static_cast<unsigned>(payload.size()),
             payload.c_str());
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        handle_wifi_legacy_payload(payload);
        return;
    }

    cJSON* command = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(command)) {
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "error", "Error parsing JSON");
        send_ble_response(response);
        cJSON_Delete(response);
        cJSON_Delete(root);
        return;
    }

    RuntimeState& s = state();
    s.provisioning_active = true;
    s.led_mode = LedMode::Provisioning;

    const std::string cmd = command->valuestring;
    bool success = true;
    std::string error;

    if (cmd == "SERVER_DATA") {
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) {
            std::lock_guard<std::mutex> lock(s.config_mutex);
            auto assign = [&](const char* key, std::string& target) {
                cJSON* item = cJSON_GetObjectItem(data, key);
                if (cJSON_IsString(item)) {
                    target = item->valuestring;
                }
            };
            assign("authUrl", s.config.auth_url);
            assign("tokenUrl", s.config.token_url);
            assign("mqttUrl", s.config.mqtt_url);
            assign("cloudApiUrl", s.config.cloud_api_url);
            assign("configUrl", s.config.config_url);
        } else {
            success = false;
            error = "Missing data object";
        }
        if (success) {
            success = save_runtime_config();
            if (!success) {
                error = "Failed to save runtime config";
            }
        }
    } else if (cmd == "GET_TOKEN") {
        RuntimeConfig cfg{};
        success = wait_for_device_code(cfg);
        if (!success) {
            error = "Timeout waiting for data!";
        }
    } else if (cmd == "CHECK_AUTH") {
        success = wait_for_access_token();
        if (success) {
            mqtt_request_reconnect();
        } else {
            error = "Can't get access token!";
        }
    } else {
        success = false;
        error = "Unknown command";
    }

    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "command", cmd.c_str());
    cJSON_AddBoolToObject(response, "isSuccess", success);
    cJSON_AddStringToObject(response, "error", error.c_str());
    cJSON_AddNumberToObject(response, "byteReceived", static_cast<double>(payload.size()));

    if (cmd == "GET_TOKEN" && success) {
        RuntimeConfig cfg{};
        {
            std::lock_guard<std::mutex> lock(s.config_mutex);
            cfg = s.config;
        }
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "chip_id", chip_id().c_str());
        cJSON_AddStringToObject(data, "mac_address", mac_address().c_str());
        cJSON_AddStringToObject(data, "user_code", cfg.user_code.c_str());
        cJSON_AddStringToObject(data, "verification_uri", cfg.verification_url.c_str());
        cJSON_AddStringToObject(data, "model_code", CORE_MODEL_CODE);
        cJSON_AddStringToObject(data, "vendor_code", CORE_VENDOR_CODE);
        cJSON_AddStringToObject(data, "version_code", CORE_FW_VERSION);
        cJSON_AddStringToObject(data, "hw_code", CORE_HW_CODE);
        cJSON_AddItemToObject(response, "data", data);
    }

    send_ble_response(response);
    cJSON_Delete(response);
    cJSON_Delete(root);

    if (cmd == "CHECK_AUTH" && success) {
        vTaskDelay(pdMS_TO_TICKS(300));
        safe_restart();
    }
}

// FreeRTOS worker that owns heavy provisioning work after BLE callback enqueue.
void ble_rx_task(void*) {
    for (;;) {
        char* msg = nullptr;
        if (xQueueReceive(g_ble_rx_queue, &msg, portMAX_DELAY) == pdTRUE && msg) {
            handle_json_payload(msg);
            free(msg);
        }
    }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
public:
    // Only copy and queue the payload here.
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        ensure_ble_rx_queue();
        if (!g_ble_rx_queue) {
            ESP_LOGW(TAG, "BLE RX queue is unavailable");
            return;
        }

        const std::string value = characteristic->getValue();
        char* buffer = static_cast<char*>(malloc(value.size() + 1));
        if (!buffer) {
            ESP_LOGW(TAG, "Failed to allocate BLE RX buffer");
            return;
        }

        std::memcpy(buffer, value.data(), value.size());
        buffer[value.size()] = '\0';
        if (xQueueSend(g_ble_rx_queue, &buffer, 0) != pdTRUE) {
            free(buffer);
            ESP_LOGW(TAG, "BLE RX queue full, dropping message");
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        state().ble_client_connected = true;
        ESP_LOGI(TAG, "BLE client connected, mtu=%u", info.getMTU());
    }

    void onDisconnect(NimBLEServer* server, NimBLEConnInfo&, int) override {
        state().ble_client_connected = false;
        ESP_LOGI(TAG, "BLE client disconnected, restarting advertising");
        server->startAdvertising();
    }
};

}  // namespace

void init_ble_service() {
    // MTU 512 is required by the existing mobile client payload size expectations.
    NimBLEDevice::init(kBleDeviceName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(512);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    NimBLEService* service = server->createService(kBleServiceUuid);
    NimBLECharacteristic* rx = service->createCharacteristic(
        kBleRxUuid,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallbacks());
    g_tx = service->createCharacteristic(kBleTxUuid, NIMBLE_PROPERTY::NOTIFY);
    service->start();

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setName(kBleDeviceName);
    advertising->addServiceUUID(kBleServiceUuid);
    advertising->enableScanResponse(true);
    advertising->start();

    ensure_ble_rx_queue();
    if (g_ble_rx_queue && !g_ble_rx_task_handle) {
        xTaskCreate(ble_rx_task, "ble_rx_task", kBleRxTaskStack, nullptr, 5, &g_ble_rx_task_handle);
    }

    ESP_LOGI(TAG, "BLE GATT server started as %s", kBleDeviceName);
}

}  // namespace core
