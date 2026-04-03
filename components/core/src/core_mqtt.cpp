#include "core.h"
#include "core_build_info.h"
#include "core_internal.h"
#include "core_mqtt_process.h"
#include "core_runtime_constants.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/ip4_addr.h>

namespace core {

namespace {

static const char* TAG = "core_mqtt";
bool g_device_info_sent = false;
// Tracks the active broker URI only for observability/logging.
std::string g_active_broker_uri{};

// Recreates the legacy URL-building behavior expected by the current cloud setup.
// The function intentionally does not invent paths; it only guarantees a WSS
// scheme and adds `:443` when the incoming provisioning value omits a port.
bool build_mqtt_uri(const std::string& raw_url, std::string& out_uri) {
    if (raw_url.empty()) {
        return false;
    }

    if (raw_url.rfind("wss://", 0) == 0) {
        out_uri = raw_url;
        return true;
    }

    if (raw_url.find(':') != std::string::npos) {
        out_uri = "wss://" + raw_url;
        return true;
    }

    out_uri = "wss://" + raw_url + ":443";
    return true;
}

void subscribe_commands(esp_mqtt_client_handle_t client) {
    subscribe_core_mqtt_routes(client, g_active_broker_uri);
}

void destroy_active_mqtt_client(RuntimeState& s) {
    esp_mqtt_client_handle_t old_client = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.mqtt_client_mutex);
        old_client = s.mqtt_client;
        s.mqtt_client = nullptr;
    }

    if (old_client) {
        esp_mqtt_client_stop(old_client);
        esp_mqtt_client_destroy(old_client);
    }
}

bool load_wifi_config(RuntimeConfig& cfg) {
    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);
    cfg = s.config;
    return !cfg.wifi_ssid.empty() && !cfg.wifi_password.empty();
}

void perform_wifi_hard_reconnect() {
    RuntimeState& s = state();
    RuntimeConfig cfg{};
    if (!load_wifi_config(cfg)) {
        ESP_LOGW(TAG, "skipping WiFi hard reconnect because credentials are incomplete");
        return;
    }

    ESP_LOGW(TAG, "performing WiFi/MQTT hard reconnect after repeated failures");
    destroy_active_mqtt_client(s);
    s.mqtt_connected = false;
    s.network_ready = false;
    s.ip_acquired = false;
    s.wifi_connected = false;
    s.led_mode = LedMode::ConnectingWifi;

    const esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_disconnect during hard reconnect failed: 0x%x", disconnect_err);
    }
    const esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop during hard reconnect failed: 0x%x", stop_err);
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    wifi_config_t wifi_cfg = {};
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), cfg.wifi_ssid.c_str(), sizeof(wifi_cfg.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), cfg.wifi_password.c_str(), sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s.last_network_recovery_us = esp_timer_get_time();
}

// Sends the post-connect identity envelope expected by the cloud side.
void publish_device_identity() {
    if (g_device_info_sent) {
        return;
    }

    char ip_str[IP4ADDR_STRLEN_MAX] = "0.0.0.0";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info{};
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ip4addr_ntoa_r(reinterpret_cast<const ip4_addr_t*>(&ip_info.ip), ip_str, sizeof(ip_str));
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* params = cJSON_CreateObject();
    if (!root || !params) {
        if (root) {
            cJSON_Delete(root);
        }
        if (params) {
            cJSON_Delete(params);
        }
        return;
    }

    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", "device-data");

    auto add_raw_object = [&](const char* key, const char* value) {
        cJSON* obj = cJSON_CreateObject();
        if (!obj) {
            return;
        }
        cJSON_AddStringToObject(obj, "raw", value ? value : "");
        cJSON_AddItemToObject(params, key, obj);
    };

    add_raw_object("sw_version", CORE_FW_VERSION);
    add_raw_object("IPv4", ip_str);
    const char* hw_code = CORE_HW_CODE;
    if (hw_code && hw_code[0] != '\0') {
        cJSON_AddStringToObject(params, "hw_version", hw_code);
    }
    add_raw_object("mac", mac_address().c_str());
    add_raw_object("model", CORE_MODEL_CODE);
    add_raw_object("vendor", CORE_VENDOR_CODE);

    cJSON_AddItemToObject(root, "params", params);
    char* encoded = cJSON_PrintUnformatted(root);
    if (encoded) {
        mqtt_enqueue_message(build_stream_topic("rpcout"), encoded, 0, false);
        cJSON_free(encoded);
        g_device_info_sent = true;
    }
    cJSON_Delete(root);
}

// Transport queue drain task.
//
// The higher-level data queue already owns semantic deduplication. This worker
// is intentionally dumb: pop the next MQTT message and publish it in order.
void mqtt_queue_task(void*) {
    RuntimeState& s = state();
    for (;;) {
        if (!s.mqtt_connected) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        QueuedMessage message{};
        bool have_message = false;
        {
            std::lock_guard<std::mutex> lock(s.mqtt_queue_mutex);
            if (!s.mqtt_queue.empty()) {
                message = s.mqtt_queue.front();
                s.mqtt_queue.pop_front();
                have_message = true;
            }
        }
        if (!have_message) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        std::lock_guard<std::mutex> client_lock(s.mqtt_client_mutex);
        if (s.mqtt_client) {
            ESP_LOGI(TAG,
                     "MQTT publish endpoint=%s topic=%s payload=%s",
                     g_active_broker_uri.c_str(),
                     message.topic.c_str(),
                     message.payload.c_str());
            esp_mqtt_client_publish(
                s.mqtt_client,
                message.topic.c_str(),
                message.payload.c_str(),
                static_cast<int>(message.payload.size()),
                message.qos,
                message.retain ? 1 : 0);
        }
    }
}

// Core-owned incoming MQTT commands.
//
// Topic-specific logic lives in `core_mqtt_process.cpp` so contributors do not
// need to edit this transport file for every new route.
void handle_command(const std::string& topic, const std::string& payload) {
    if (!handle_core_mqtt_route(topic, payload)) {
        ESP_LOGW(TAG, "MQTT recv topic has no configured handler topic=%s", topic.c_str());
    }
}

// esp-mqtt event adapter.
// This is the only place where MQTT transport events are translated into core
// runtime state transitions.
void mqtt_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            mqtt_handle_connected();
            subscribe_commands(event->client);
            publish_device_identity();
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG,
                     "MQTT subscribed endpoint=%s msg_id=%d",
                     g_active_broker_uri.c_str(),
                     event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGW(TAG,
                     "MQTT unsubscribed endpoint=%s msg_id=%d",
                     g_active_broker_uri.c_str(),
                     event->msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_handle_disconnected();
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG,
                     "MQTT recv endpoint=%s topic=%.*s payload=%.*s",
                     g_active_broker_uri.c_str(),
                     event->topic_len,
                     event->topic,
                     event->data_len,
                     event->data);
            handle_command(
                std::string(event->topic, static_cast<size_t>(event->topic_len)),
                std::string(event->data, static_cast<size_t>(event->data_len)));
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG,
                     "MQTT error endpoint=%s type=%d connect_ret=%d esp_tls_last_esp_err=0x%x esp_tls_stack_err=0x%x sock_errno=%d",
                     g_active_broker_uri.c_str(),
                     event->error_handle ? event->error_handle->error_type : -1,
                     event->error_handle ? event->error_handle->connect_return_code : -1,
                     event->error_handle ? event->error_handle->esp_tls_last_esp_err : 0,
                     event->error_handle ? event->error_handle->esp_tls_stack_err : 0,
                     event->error_handle ? event->error_handle->esp_transport_sock_errno : 0);
            break;
        default:
            break;
    }
}

// Reconnect supervisor.
//
// It rebuilds the MQTT client from current runtime config rather than trying to
// mutate a long-lived client in place. That keeps provisioning updates and token
// refresh semantics straightforward for contributors.
void mqtt_reconnect_task(void*) {
    RuntimeState& s = state();
    for (;;) {
        if (!s.network_ready || s.mqtt_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        RuntimeConfig cfg{};
        {
            std::lock_guard<std::mutex> lock(s.config_mutex);
            cfg = s.config;
        }
        if (cfg.access_token.empty() || cfg.mqtt_url.empty()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::string broker_uri;
        if (!build_mqtt_uri(cfg.mqtt_url, broker_uri)) {
            s.led_mode = LedMode::Error;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        s.led_mode = LedMode::ConnectingMqtt;
        const std::string client_id = "ESP32#" + chip_id();
        esp_mqtt_client_config_t mqtt_cfg = {};
        mqtt_cfg.broker.address.uri = broker_uri.c_str();
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqtt_cfg.credentials.username = "device-token";
        mqtt_cfg.credentials.authentication.password = cfg.access_token.c_str();
        mqtt_cfg.credentials.client_id = client_id.c_str();
        mqtt_cfg.network.timeout_ms = 3000;
        mqtt_cfg.network.reconnect_timeout_ms = 5000;
        mqtt_cfg.session.keepalive = 15;
        mqtt_cfg.session.disable_clean_session = false;
        mqtt_cfg.task.stack_size = 3584;
        g_active_broker_uri = broker_uri;

        ESP_LOGI(TAG,
                 "MQTT connect raw_url=%s uri=%s keepalive=%d timeout_ms=%d reconnect_timeout_ms=%d clean_session=%d stack=%d",
                 cfg.mqtt_url.c_str(),
                 broker_uri.c_str(),
                 mqtt_cfg.session.keepalive,
                 mqtt_cfg.network.timeout_ms,
                 mqtt_cfg.network.reconnect_timeout_ms,
                 mqtt_cfg.session.disable_clean_session ? 0 : 1,
                 mqtt_cfg.task.stack_size);

        destroy_active_mqtt_client(s);

        esp_mqtt_client_handle_t new_client = esp_mqtt_client_init(&mqtt_cfg);
        if (new_client) {
            esp_mqtt_client_register_event(
                new_client,
                static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                mqtt_event_handler,
                nullptr);
            {
                std::lock_guard<std::mutex> lock(s.mqtt_client_mutex);
                s.mqtt_client = new_client;
            }
            esp_mqtt_client_start(new_client);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));

        if (s.mqtt_connected) {
            s.consecutive_mqtt_failures = 0;
            continue;
        }

        const int failures = s.consecutive_mqtt_failures.fetch_add(1) + 1;
        ESP_LOGW(TAG, "MQTT reconnect attempt failed (consecutive_failures=%d)", failures);

        if (failures >= kTokenRefreshFailureThreshold && !cfg.refresh_token.empty()) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t last_attempt_us = s.last_token_refresh_attempt_us.load();
            if (last_attempt_us == 0 || (now_us - last_attempt_us) >= kTokenRefreshCooldownUs) {
                ESP_LOGW(TAG, "MQTT reconnect threshold reached, forcing token refresh");
                s.last_token_refresh_attempt_us = now_us;
                if (perform_token_refresh()) {
                    s.consecutive_mqtt_failures = 0;
                    continue;
                }
            }
        }

        if (failures >= kNetworkRecoveryFailureThreshold) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t last_recovery_us = s.last_network_recovery_us.load();
            if (last_recovery_us == 0 || (now_us - last_recovery_us) >= kNetworkRecoveryCooldownUs) {
                s.consecutive_mqtt_failures = 0;
                perform_wifi_hard_reconnect();
                continue;
            }
        }
    }
}

}  // namespace

// Pushes a transport-ready MQTT message into the bounded RAM queue.
void mqtt_enqueue_message(std::string topic, std::string payload, int qos, bool retain) {
    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.mqtt_queue_mutex);
    if (s.mqtt_queue.size() >= kMqttQueueLimit) {
        s.mqtt_queue.pop_front();
    }
    s.mqtt_queue.push_back(QueuedMessage{
        std::move(topic),
        std::move(payload),
        qos,
        retain,
    });
}

void mqtt_request_reconnect() {
    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.mqtt_client_mutex);
    if (s.mqtt_client) {
        esp_mqtt_client_reconnect(s.mqtt_client);
    }
}

void mqtt_handle_connected() {
    RuntimeState& s = state();
    s.mqtt_connected = true;
    s.consecutive_mqtt_failures = 0;
    s.led_mode = LedMode::Online;
}

void mqtt_handle_disconnected() {
    RuntimeState& s = state();
    s.mqtt_connected = false;
    g_device_info_sent = false;
    g_active_broker_uri.clear();
    s.led_mode = s.network_ready ? LedMode::ConnectingMqtt : LedMode::ConnectingWifi;
}

void wifi_request_hard_reconnect() {
    perform_wifi_hard_reconnect();
}

void init_wifi_stack() {
    // The core currently owns the Wi-Fi STA event loop because provisioning,
    // token flows, and MQTT all depend on shared connectivity state.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    auto wifi_event = [](void*, esp_event_base_t base, int32_t event_id, void*) {
        RuntimeState& s = state();
        if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            s.led_mode = LedMode::ConnectingWifi;
            esp_wifi_connect();
        } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
            s.wifi_connected = true;
        } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s.wifi_connected = false;
            s.ip_acquired = false;
            s.network_ready = false;
            s.mqtt_connected = false;
            s.led_mode = LedMode::ConnectingWifi;
            esp_wifi_connect();
        } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            s.ip_acquired = true;
            s.network_ready = true;
            s.led_mode = LedMode::ConnectingMqtt;
            mqtt_request_reconnect();
        }
    };

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
}

void connect_wifi_if_configured() {
    RuntimeState& s = state();
    RuntimeConfig cfg{};
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        cfg = s.config;
    }
    if (cfg.wifi_ssid.empty()) {
        s.led_mode = LedMode::Provisioning;
        return;
    }

    wifi_config_t wifi_cfg = {};
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), cfg.wifi_ssid.c_str(), sizeof(wifi_cfg.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), cfg.wifi_password.c_str(), sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void mqtt_start_tasks() {
    data_queue_start_task();
    xTaskCreate(mqtt_queue_task, "core_mqtt_q", 4096, nullptr, 4, nullptr);
    xTaskCreate(mqtt_reconnect_task, "core_mqtt_rc", 4096, nullptr, 4, nullptr);
}

}  // namespace core
