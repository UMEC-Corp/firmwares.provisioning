#include "core.h"
#include "core_build_info.h"
#include "core_internal.h"
#include "core_runtime_constants.h"

#include <cstdio>
#include <ctime>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

namespace core {

namespace {

static const char* TAG = "core_app";
RuntimeState g_state{};
// Maintenance intervals are intentionally long-running and conservative.
// These tasks are not control-plane heartbeats; they are background upkeep
// mechanisms that should not fight with MQTT traffic or consume bandwidth.
TaskHandle_t g_time_sync_task = nullptr;
TaskHandle_t g_config_refresh_task = nullptr;

void time_sync_task(void*) {
    ESP_LOGI(TAG, "running periodic time sync");
    (void)sync_time_with_sntp();
    g_time_sync_task = nullptr;
    vTaskDelete(nullptr);
}

void config_refresh_task(void*) {
    ESP_LOGI(TAG, "running periodic remote config refresh");
    (void)fetch_and_store_remote_config();
    g_config_refresh_task = nullptr;
    vTaskDelete(nullptr);
}

// Periodic maintenance coordinator.
//
// Responsibilities:
// - keep token refresh alive when MQTT is down
// - synchronize time so telemetry can carry valid Unix timestamps
// - periodically refresh remote URLs from `configUrl`
//
// This task intentionally owns only orchestration. The actual protocol work
// lives in dedicated HTTP/NTP helpers so future ports can replace those pieces
// independently.
void maintenance_task(void*) {
    RuntimeState& s = state();
    int64_t last_config_refresh_us = 0;
    int64_t last_time_sync_us = 0;

    for (;;) {
        if (s.network_ready) {
            const int64_t now_us = esp_timer_get_time();
            RuntimeConfig cfg{};
            {
                std::lock_guard<std::mutex> lock(s.config_mutex);
                cfg = s.config;
            }
            if (cfg.access_token.empty() && !cfg.auth_url.empty()) {
                s.led_mode = LedMode::WaitingToken;
            } else if (!cfg.refresh_token.empty()) {
                const int64_t last_success_us = s.last_token_refresh_success_us.load();
                const int64_t last_attempt_us = s.last_token_refresh_attempt_us.load();
                const bool refresh_timing_unknown =
                    !cfg.access_token.empty() && cfg.token_expires_in > 0 && last_success_us == 0;
                const bool proactive_refresh_due =
                    cfg.token_expires_in > 0 &&
                    last_success_us > 0 &&
                    (now_us - last_success_us) >=
                        (static_cast<int64_t>(cfg.token_expires_in) * 1000000LL * 10LL) / 11LL;
                const bool disconnected_refresh_due = !s.mqtt_connected.load();
                const bool cooldown_elapsed =
                    last_attempt_us == 0 || (now_us - last_attempt_us) >= kTokenRefreshCooldownUs;
                if ((disconnected_refresh_due || proactive_refresh_due || refresh_timing_unknown) &&
                    cooldown_elapsed) {
                    s.last_token_refresh_attempt_us = now_us;
                    if (!perform_token_refresh()) {
                        ESP_LOGW(TAG,
                                 "token refresh attempt failed (mqtt_connected=%d expires_in=%ld timing_unknown=%d)",
                                 s.mqtt_connected.load() ? 1 : 0,
                                 static_cast<long>(cfg.token_expires_in),
                                 refresh_timing_unknown ? 1 : 0);
                    }
                }
            }

            if ((last_time_sync_us == 0 || (now_us - last_time_sync_us) >= kTimeSyncPeriodUs) &&
                g_time_sync_task == nullptr) {
                xTaskCreate(time_sync_task, "core_time_sync", kTimeSyncTaskStack, nullptr, 3, &g_time_sync_task);
                last_time_sync_us = now_us;
            }

            if (!cfg.config_url.empty() &&
                (last_config_refresh_us == 0 || (now_us - last_config_refresh_us) >= kConfigRefreshPeriodUs) &&
                g_config_refresh_task == nullptr) {
                xTaskCreate(config_refresh_task, "core_cfg_refresh", kConfigRefreshTaskStack, nullptr, 3, &g_config_refresh_task);
                last_config_refresh_us = now_us;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

}  // namespace

RuntimeState& state() {
    return g_state;
}

// Uses the Wi-Fi STA MAC as the canonical controller identity.
// The same identity is exposed over BLE and MQTT so mobile/cloud tooling sees
// one consistent device identifier.
std::string chip_id() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buffer[13] = {};
    std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buffer;
}

std::string mac_address() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buffer[18] = {};
    std::snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buffer;
}

// Returns current Unix time as understood by the system clock.
uint32_t unix_timestamp() {
    time_t now = 0;
    time(&now);
    return static_cast<uint32_t>(now);
}

// Treats timestamps before 2017-01-01 as "not synchronized".
// This protects telemetry and cloud state from obviously invalid boot-time
// values before NVS restore or SNTP synchronization completes.
bool has_valid_time() {
    return unix_timestamp() >= 1483228800U;
}

std::string build_command_topic(const char* suffix) {
    return "command/" + chip_id() + "/" + suffix;
}

std::string build_stream_topic(const char* suffix) {
    return "stream/" + chip_id() + "/" + suffix;
}

void safe_restart() {
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void init(const CoreHardwareConfig& hardware) {
    // Hardware pin ownership is fixed at init and then treated as immutable.
    RuntimeState& s = state();
    s.button_pin = hardware.button_pin;
    s.status_led_pin = hardware.status_led_pin;
    s.activity_led_pin = hardware.activity_led_pin;
    s.led_mode = LedMode::Provisioning;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    load_runtime_config();
    if (!s.config.access_token.empty() && s.config.token_expires_in > 0) {
        s.last_token_refresh_success_us = 0;
        s.last_token_refresh_attempt_us = 0;
    }
    restore_time_from_nvs();
    init_button_and_leds();
    init_wifi_stack();
    init_ble_service();
    connect_wifi_if_configured();
    mqtt_start_tasks();
    xTaskCreate(maintenance_task, "core_maint", kMaintenanceTaskStack, nullptr, 3, nullptr);

    ESP_LOGI(TAG, "Core initialized for %s", CORE_MODEL_CODE);
}

void __attribute__((weak)) app_handle_set_temp(float value) {
    ESP_LOGI(TAG, "No app override for set-temp, value=%.2f", static_cast<double>(value));
}

}  // namespace core
