#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include <esp_err.h>
#include <mqtt_client.h>

namespace core {

// NVS-backed runtime configuration shared by provisioning, HTTP, and MQTT code.
//
// This structure intentionally contains only persisted cloud/bootstrap state.
// Short-lived transport/runtime flags belong in `RuntimeState`.
struct RuntimeConfig {
    std::string wifi_ssid{};
    std::string wifi_password{};
    std::string auth_url{};
    std::string token_url{};
    std::string mqtt_url{};
    std::string cloud_api_url{};
    std::string config_url{};
    std::string access_token{};
    std::string refresh_token{};
    std::string device_code{};
    std::string user_code{};
    std::string verification_url{};
    int32_t token_expires_in{0};
};

// Transport-level MQTT message buffered after higher-level telemetry processing.
struct QueuedMessage {
    std::string topic;
    std::string payload;
    int qos{0};
    bool retain{false};
};

// High-level local indicator states.
//
// `Auto` means "derive LEDs from runtime state". Other values are forced modes
// used for local UX moments such as button hold feedback.
enum class LedMode : uint8_t {
    Off = 0,
    Auto,
    ButtonHold,
    Provisioning,
    ConnectingWifi,
    WaitingToken,
    ConnectingMqtt,
    Online,
    Error,
};

// Global runtime singleton owned by the core.
//
// The component intentionally centralizes mutable state here so that:
// - provisioning, MQTT, HTTP, and local UX share one bounded state surface
// - concurrency is explicit via mutexes/atomics
// - future ports can replace subsystems without hidden globals
struct RuntimeState {
    RuntimeConfig config{};
    std::mutex config_mutex{};
    std::deque<QueuedMessage> mqtt_queue{};
    std::mutex mqtt_queue_mutex{};
    esp_mqtt_client_handle_t mqtt_client{nullptr};
    std::mutex mqtt_client_mutex{};
    std::atomic<bool> wifi_connected{false};
    std::atomic<bool> ip_acquired{false};
    std::atomic<bool> network_ready{false};
    std::atomic<bool> mqtt_connected{false};
    std::atomic<bool> ble_client_connected{false};
    std::atomic<bool> provisioning_active{false};
    std::atomic<bool> ota_in_progress{false};
    std::atomic<LedMode> led_mode{LedMode::Auto};
    std::atomic<int64_t> last_token_refresh_attempt_us{0};
    std::atomic<int64_t> last_token_refresh_success_us{0};
    std::atomic<int32_t> consecutive_mqtt_failures{0};
    std::atomic<int64_t> last_network_recovery_us{0};
    int button_pin{0};
    int status_led_pin{2};
    int activity_led_pin{-1};
};

// Accessor for the single runtime instance allocated in `core_app.cpp`.
RuntimeState& state();
// Stable chip identity helpers used for MQTT topics and provisioning payloads.
std::string chip_id();
std::string mac_address();
// Current Unix time view used by telemetry formatting.
uint32_t unix_timestamp();
// Returns true once time is sane enough for externally visible telemetry.
bool has_valid_time();
// Topic builders for the UMEC cloud contract.
std::string build_command_topic(const char* suffix);
std::string build_stream_topic(const char* suffix);
// Reboots the MCU after first stopping the Wi-Fi driver cleanly.
void safe_restart();

// Persistent configuration operations.
void load_runtime_config();
bool save_runtime_config();
void clear_runtime_credentials();

// Local UX / transport / maintenance subsystem entrypoints.
void init_button_and_leds();
void init_ble_service();
void init_wifi_stack();
void connect_wifi_if_configured();
void data_queue_start_task();
// Time persistence and synchronization.
void restore_time_from_nvs();
void save_time_to_nvs(uint32_t unix_time);
bool sync_time_with_sntp();
// MQTT transport helpers.
void mqtt_request_reconnect();
void mqtt_enqueue_message(std::string topic, std::string payload, int qos = 0, bool retain = false);
void mqtt_handle_connected();
void mqtt_handle_disconnected();
void wifi_request_hard_reconnect();

// HTTP-backed cloud flows.
bool perform_device_code_exchange();
bool perform_token_exchange();
bool perform_token_refresh();
bool fetch_and_store_remote_config();
esp_err_t perform_ota_update(const char* url);

}  // namespace core
