#include "core_internal.h"

#include <nvs.h>

namespace core {

namespace {

constexpr const char* kCoreNs = "corecfg";
constexpr const char* kWifiNs = "wifi";

// Safe string fetch helper for NVS variable-length strings.
std::string nvs_get_string(nvs_handle_t handle, const char* key) {
    size_t required = 0;
    if (nvs_get_str(handle, key, nullptr, &required) != ESP_OK || required == 0) {
        return {};
    }
    std::string value(required, '\0');
    if (nvs_get_str(handle, key, value.data(), &required) != ESP_OK) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

// Empty values erase the key so NVS remains compact and semantically clean.
void nvs_write_string(nvs_handle_t handle, const char* key, const std::string& value) {
    if (value.empty()) {
        nvs_erase_key(handle, key);
    } else {
        nvs_set_str(handle, key, value.c_str());
    }
}

}  // namespace

// Loads the full persisted cloud/runtime configuration into memory.
void load_runtime_config() {
    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);

    nvs_handle_t wifi = 0;
    if (nvs_open(kWifiNs, NVS_READONLY, &wifi) == ESP_OK) {
        s.config.wifi_ssid = nvs_get_string(wifi, "ssid");
        s.config.wifi_password = nvs_get_string(wifi, "password");
        nvs_close(wifi);
    }

    nvs_handle_t core = 0;
    if (nvs_open(kCoreNs, NVS_READONLY, &core) == ESP_OK) {
        s.config.auth_url = nvs_get_string(core, "auth_url");
        s.config.token_url = nvs_get_string(core, "token_url");
        s.config.mqtt_url = nvs_get_string(core, "mqtt_url");
        s.config.cloud_api_url = nvs_get_string(core, "cloud_api");
        s.config.config_url = nvs_get_string(core, "config_url");
        s.config.access_token = nvs_get_string(core, "access_tok");
        s.config.refresh_token = nvs_get_string(core, "refresh_tok");
        s.config.device_code = nvs_get_string(core, "device_code");
        s.config.user_code = nvs_get_string(core, "user_code");
        s.config.verification_url = nvs_get_string(core, "verify_url");
        nvs_get_i32(core, "expires_in", &s.config.token_expires_in);
        nvs_close(core);
    }
}

// Persists the in-memory runtime configuration back to NVS.
bool save_runtime_config() {
    RuntimeState& s = state();
    std::lock_guard<std::mutex> lock(s.config_mutex);

    nvs_handle_t wifi = 0;
    if (nvs_open(kWifiNs, NVS_READWRITE, &wifi) != ESP_OK) {
        return false;
    }
    nvs_write_string(wifi, "ssid", s.config.wifi_ssid);
    nvs_write_string(wifi, "password", s.config.wifi_password);
    if (nvs_commit(wifi) != ESP_OK) {
        nvs_close(wifi);
        return false;
    }
    nvs_close(wifi);

    nvs_handle_t core = 0;
    if (nvs_open(kCoreNs, NVS_READWRITE, &core) != ESP_OK) {
        return false;
    }
    nvs_write_string(core, "auth_url", s.config.auth_url);
    nvs_write_string(core, "token_url", s.config.token_url);
    nvs_write_string(core, "mqtt_url", s.config.mqtt_url);
    nvs_write_string(core, "cloud_api", s.config.cloud_api_url);
    nvs_write_string(core, "config_url", s.config.config_url);
    nvs_write_string(core, "access_tok", s.config.access_token);
    nvs_write_string(core, "refresh_tok", s.config.refresh_token);
    nvs_write_string(core, "device_code", s.config.device_code);
    nvs_write_string(core, "user_code", s.config.user_code);
    nvs_write_string(core, "verify_url", s.config.verification_url);
    nvs_set_i32(core, "expires_in", s.config.token_expires_in);
    const esp_err_t commit = nvs_commit(core);
    nvs_close(core);
    return commit == ESP_OK;
}

// Clears all provisioned runtime credentials and endpoints.
// This is the factory-style reset path owned by the local button and MQTT reset command.
void clear_runtime_credentials() {
    RuntimeState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.config_mutex);
        s.config = RuntimeConfig{};
    }
    save_runtime_config();
}

}  // namespace core
