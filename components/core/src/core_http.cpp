#include "core_internal.h"
#include "core_runtime_constants.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sys/time.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

namespace core {

namespace {

static const char* TAG = "core_http";
// HTTP response collector used by esp_http_client callback mode.
struct ResponseBuffer {
    std::string body;
};

// Accepts URLs from provisioning/config with or without explicit HTTP scheme.
std::string normalize_http_url(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    if (!value.empty() && value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0) {
        value = "https://" + value;
    }
    return value;
}

// Keeps HTTP body accumulation centralized and bounded.
esp_err_t http_event_handler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data && event->data_len > 0) {
        auto* buffer = static_cast<ResponseBuffer*>(event->user_data);
        if (buffer->body.size() < 8192) {
            buffer->body.append(static_cast<const char*>(event->data), static_cast<size_t>(event->data_len));
        }
    }
    return ESP_OK;
}

// Shared HTTP request helper used by token flows, config refresh, and OTA setup.
bool send_http_request(const std::string& url,
                       esp_http_client_method_t method,
                       const std::string& payload,
                       std::string& response,
                       int& status_code,
                       bool with_bearer) {
    const std::string normalized = normalize_http_url(url);
    if (normalized.empty()) {
        return false;
    }

    ResponseBuffer buffer{};
    esp_http_client_config_t config = {};
    config.url = normalized.c_str();
    config.method = method;
    config.timeout_ms = 5000;
    config.user_data = &buffer;
    config.event_handler = http_event_handler;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    RuntimeState& s = state();

    ESP_LOGI(TAG,
             "HTTP TLS preflight now=%lu has_valid_time=%d",
             static_cast<unsigned long>(unix_timestamp()),
             has_valid_time() ? 1 : 0);
    ESP_LOGI(TAG, "HTTP request method=%s url=%s",
             method == HTTP_METHOD_GET ? "GET" : "POST",
             normalized.c_str());
    if (!payload.empty()) {
        ESP_LOGI(TAG, "HTTP payload=%s", payload.c_str());
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "UMEC-Core/0.1");
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, payload.c_str(), static_cast<int>(payload.size()));
    }

    if (with_bearer) {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        if (!s.config.access_token.empty()) {
            const std::string auth = "Bearer " + s.config.access_token;
            esp_http_client_set_header(client, "Authorization", auth.c_str());
        }
    }

    const esp_err_t err = esp_http_client_perform(client);
    status_code = esp_http_client_get_status_code(client);
    response = buffer.body;
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP perform err=%s (0x%x)", esp_err_to_name(err), static_cast<unsigned int>(err));
    ESP_LOGI(TAG, "HTTP response status=%d body=%s",
             status_code,
             response.empty() ? "<empty>" : response.c_str());
    return err == ESP_OK && status_code >= 200 && status_code < 300;
}

}  // namespace

// Persists a reconstructed "good" Unix time so telemetry can recover after reboot
// before SNTP completes again.
void save_time_to_nvs(uint32_t unix_time) {
    nvs_handle_t handle = 0;
    if (nvs_open("time", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u64(handle, "saved_time", static_cast<uint64_t>(unix_time));
    nvs_set_u64(handle, "rtc_offset", static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL));
    nvs_commit(handle);
    nvs_close(handle);
}

// Restores the persisted Unix time snapshot on boot.
// This does not guarantee perfect accuracy; it only guarantees telemetry starts
// from a sane wall-clock baseline until the next SNTP sync.
void restore_time_from_nvs() {
    nvs_handle_t handle = 0;
    if (nvs_open("time", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint64_t saved_time = 0;
    uint64_t rtc_offset = 0;
    nvs_get_u64(handle, "saved_time", &saved_time);
    nvs_get_u64(handle, "rtc_offset", &rtc_offset);
    nvs_close(handle);

    if (saved_time == 0 || rtc_offset == 0) {
        return;
    }

    const uint64_t now_offset = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    const time_t restored = static_cast<time_t>(saved_time + (now_offset - rtc_offset));
    timeval tv = {};
    tv.tv_sec = restored;
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "restored time from NVS: %lu", static_cast<unsigned long>(restored));
}

// Performs a bounded SNTP synchronization pass.
// The helper returns quickly on failure instead of blocking indefinitely.
bool sync_time_with_sntp() {
    static bool s_sntp_started = false;

    if (s_sntp_started) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, const_cast<char*>("time.nist.gov"));
    esp_sntp_setservername(1, const_cast<char*>("pool.ntp.org"));
    esp_sntp_setservername(2, const_cast<char*>("time.google.com"));
    esp_sntp_init();
    s_sntp_started = true;

    const int64_t start_sec = esp_timer_get_time() / 1000000LL;
    time_t now = 0;
    struct tm timeinfo = {};

    while ((esp_timer_get_time() / 1000000LL) - start_sec < kNtpTimeoutSec) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2016 - 1900)) {
            save_time_to_nvs(static_cast<uint32_t>(now));
            esp_sntp_stop();
            ESP_LOGI(TAG, "NTP time sync successful: %lu", static_cast<unsigned long>(now));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_sntp_stop();
    ESP_LOGW(TAG, "NTP time sync failed");
    return false;
}

// Device-code bootstrap step used by BLE `GET_TOKEN`.
bool perform_device_code_exchange() {
    RuntimeState& s = state();
    std::string auth_url;
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        auth_url = s.config.auth_url;
    }
    if (auth_url.empty()) {
        return false;
    }

    std::string response;
    int status = 0;
    if (!send_http_request(auth_url, HTTP_METHOD_POST, "client_id=controller01&scopes=mqtt-streaming", response, status, false)) {
        return false;
    }

    cJSON* json = cJSON_Parse(response.c_str());
    if (!json) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        cJSON* device_code = cJSON_GetObjectItem(json, "device_code");
        cJSON* user_code = cJSON_GetObjectItem(json, "user_code");
        cJSON* verify_url = cJSON_GetObjectItem(json, "verification_uri_complete");
        if (cJSON_IsString(device_code)) s.config.device_code = device_code->valuestring;
        if (cJSON_IsString(user_code)) s.config.user_code = user_code->valuestring;
        if (cJSON_IsString(verify_url)) s.config.verification_url = verify_url->valuestring;
    }
    cJSON_Delete(json);
    save_runtime_config();
    return true;
}

// Exchanges device_code for access/refresh tokens.
bool perform_token_exchange() {
    RuntimeState& s = state();
    std::string token_url;
    std::string device_code;
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        token_url = s.config.token_url;
        device_code = s.config.device_code;
    }
    if (token_url.empty() || device_code.empty()) {
        return false;
    }

    const std::string payload =
        "client_id=controller01&grant_type=urn:ietf:params:oauth:grant-type:device_code&scopes=mqtt-streaming&device_code=" +
        device_code;
    std::string response;
    int status = 0;
    if (!send_http_request(token_url, HTTP_METHOD_POST, payload, response, status, false)) {
        return false;
    }

    cJSON* json = cJSON_Parse(response.c_str());
    if (!json) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        cJSON* access = cJSON_GetObjectItem(json, "access_token");
        cJSON* refresh = cJSON_GetObjectItem(json, "refresh_token");
        cJSON* expires = cJSON_GetObjectItem(json, "expires_in");
        if (cJSON_IsString(access)) s.config.access_token = access->valuestring;
        if (cJSON_IsString(refresh)) s.config.refresh_token = refresh->valuestring;
        if (cJSON_IsNumber(expires)) s.config.token_expires_in = expires->valueint;
    }
    cJSON_Delete(json);
    s.last_token_refresh_success_us = esp_timer_get_time();
    s.last_token_refresh_attempt_us = s.last_token_refresh_success_us.load();
    save_runtime_config();
    return true;
}

// Refresh-token path used by background maintenance when MQTT is disconnected.
bool perform_token_refresh() {
    RuntimeState& s = state();
    std::string token_url;
    std::string refresh_token;
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        token_url = s.config.token_url;
        refresh_token = s.config.refresh_token;
    }
    if (token_url.empty() || refresh_token.empty()) {
        return false;
    }

    const std::string payload =
        "client_id=controller01&grant_type=refresh_token&refresh_token=" + refresh_token;
    std::string response;
    int status = 0;
    if (!send_http_request(token_url, HTTP_METHOD_POST, payload, response, status, false)) {
        return false;
    }

    cJSON* json = cJSON_Parse(response.c_str());
    if (!json) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        cJSON* access = cJSON_GetObjectItem(json, "access_token");
        cJSON* refresh = cJSON_GetObjectItem(json, "refresh_token");
        cJSON* expires = cJSON_GetObjectItem(json, "expires_in");
        if (cJSON_IsString(access)) s.config.access_token = access->valuestring;
        if (cJSON_IsString(refresh)) s.config.refresh_token = refresh->valuestring;
        if (cJSON_IsNumber(expires)) s.config.token_expires_in = expires->valueint;
    }
    cJSON_Delete(json);
    s.last_token_refresh_success_us = esp_timer_get_time();
    s.last_token_refresh_attempt_us = s.last_token_refresh_success_us.load();
    save_runtime_config();
    return true;
}

// Pulls the remote platform config and updates locally stored URLs when changed.
bool fetch_and_store_remote_config() {
    RuntimeState& s = state();
    std::string config_url;
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        config_url = s.config.config_url;
    }
    if (config_url.empty()) {
        return false;
    }

    ESP_LOGI(TAG, "fetching remote config from %s", config_url.c_str());
    std::string response;
    int status = 0;
    if (!send_http_request(config_url, HTTP_METHOD_GET, {}, response, status, true)) {
        ESP_LOGW(TAG, "remote config fetch failed status=%d body=%s",
                 status,
                 response.empty() ? "<empty>" : response.c_str());
        return false;
    }

    cJSON* json = cJSON_Parse(response.c_str());
    if (!json) {
        return false;
    }

    std::string change_log;
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        auto assign = [&](const char* key, std::string& target) {
            cJSON* item = cJSON_GetObjectItem(json, key);
            if (!cJSON_IsString(item)) {
                return;
            }
            const std::string next = item->valuestring;
            if (target != next) {
                if (!change_log.empty()) {
                    change_log += "; ";
                }
                change_log += key;
                change_log += ": ";
                change_log += target.empty() ? "<empty>" : target;
                change_log += " -> ";
                change_log += next.empty() ? "<empty>" : next;
                target = next;
            }
        };
        assign("authUrl", s.config.auth_url);
        assign("tokenUrl", s.config.token_url);
        assign("mqttUrl", s.config.mqtt_url);
        assign("cloudApiUrl", s.config.cloud_api_url);
        assign("configUrl", s.config.config_url);
    }
    cJSON_Delete(json);
    save_runtime_config();
    if (change_log.empty()) {
        ESP_LOGI(TAG, "remote config fetched from %s with no changes", config_url.c_str());
    } else {
        ESP_LOGI(TAG, "remote config updated from %s changes=%s",
                 config_url.c_str(),
                 change_log.c_str());
    }
    return true;
}

// OTA download/write path.
// The function only stages the next firmware and switches the boot partition;
// reboot remains the caller's decision.
esp_err_t perform_ota_update(const char* url) {
    RuntimeState& s = state();
    if (!url || !url[0] || s.ota_in_progress.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        s.ota_in_progress = false;
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        s.ota_in_progress = false;
        return err;
    }

    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        s.ota_in_progress = false;
        return err;
    }

    uint8_t buffer[1024];
    while (true) {
        const int read = esp_http_client_read(client, reinterpret_cast<char*>(buffer), sizeof(buffer));
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        err = esp_ota_write(ota_handle, buffer, static_cast<size_t>(read));
        if (err != ESP_OK) {
            break;
        }
    }

    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(partition);
        }
    } else {
        esp_ota_abort(ota_handle);
    }

    esp_http_client_cleanup(client);
    s.ota_in_progress = false;
    return err;
}

}  // namespace core
