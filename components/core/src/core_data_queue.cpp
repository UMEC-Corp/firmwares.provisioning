#include "core_data_queue.h"

#include "core_internal.h"
#include "core_runtime_constants.h"
#include "core_thresholds.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace core {

namespace {

static const char* TAG = "core_data_queue";

// Logical telemetry backlog keyed by method/device/parameter.
// This queue holds semantic items rather than MQTT payloads so producers can be
// simple and transport-independent.
std::map<std::string, std::unique_ptr<DataItem>> g_data_queue{};
std::mutex g_data_queue_mutex{};
// Last-sent caches drive deduplication and threshold behavior.
// Numeric and string values are tracked separately because they follow different
// cloud semantics.
std::map<std::string, float> g_last_sent_values{};
std::map<std::string, std::string> g_last_sent_string_values{};

// Queue deduplication key:
// one pending item per method/device/parameter triple.
std::string build_item_key(const DataItem& item) {
    const std::string device_id = item.get_device_id().empty() ? chip_id() : item.get_device_id();
    return item.get_method_type() + "|" + device_id + "|" + item.get_param_name();
}

// Last-sent comparison key.
// Device id is included when producers explicitly scope metrics to sub-devices.
std::string build_value_key(const DataItem& item) {
    if (!item.get_device_id().empty()) {
        return item.get_device_id() + "|" + item.get_param_name();
    }
    return item.get_param_name();
}

bool is_numeric_string(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    char* endptr = nullptr;
    std::strtof(value.c_str(), &endptr);
    return endptr && *endptr == '\0';
}

// Prefix matching helper for threshold-rule configuration.
bool starts_with(const std::string& value, const char* prefix) {
    if (!prefix) {
        return false;
    }
    const size_t prefix_len = std::char_traits<char>::length(prefix);
    return value.size() >= prefix_len && value.compare(0, prefix_len, prefix) == 0;
}

// Returns the first configured threshold rule matching the metric name.
// Rule ordering in `core_thresholds.h` therefore matters.
const ThresholdRule* find_threshold_rule(const std::string& param_name) {
    for (const auto& rule : kTelemetryThresholdRules) {
        if (!rule.pattern || rule.pattern[0] == '\0') {
            continue;
        }
        if (rule.match == ThresholdMatchKind::Exact && param_name == rule.pattern) {
            return &rule;
        }
        if (rule.match == ThresholdMatchKind::Prefix && starts_with(param_name, rule.pattern)) {
            return &rule;
        }
    }
    return nullptr;
}

// Unified numeric-send decision.
//
// The queue consults declarative rules first and falls back to the legacy
// relative-percent behavior when no explicit rule exists.
bool should_send_numeric(const std::string& param_name, float last_value, float new_value) {
    const float abs_diff = std::abs(new_value - last_value);

    if (const ThresholdRule* rule = find_threshold_rule(param_name)) {
        switch (rule->behavior) {
            case ThresholdBehavior::AbsoluteDelta:
                return abs_diff >= rule->threshold;
            case ThresholdBehavior::RelativePercent:
                return last_value != 0.0f && ((abs_diff / last_value) * 100.0f >= rule->threshold);
            case ThresholdBehavior::OnChange:
                return new_value != last_value;
        }
    }

    return last_value != 0.0f && ((abs_diff / last_value) * 100.0f >= 5.0f);
}

// Main dataQueue drain/process step.
//
// Important invariants:
// - nothing is published before MQTT is connected
// - nothing is published before time is valid
// - producers are decoupled from MQTT topic/payload formatting
// - method/parameter grouping happens here, not in producers
void process_data_queue_once() {
    RuntimeState& s = state();
    if (!s.mqtt_connected) {
        return;
    }
    if (!has_valid_time()) {
        ESP_LOGI(TAG, "Skipping telemetry publish until time is synchronized");
        return;
    }

    std::map<std::string, std::unique_ptr<DataItem>> local_queue{};
    {
        std::lock_guard<std::mutex> lock(g_data_queue_mutex);
        if (g_data_queue.empty()) {
            return;
        }
        local_queue = std::move(g_data_queue);
    }

    static std::map<std::string, bool> method_should_be_sent{};
    std::map<std::string, std::vector<std::pair<std::string, float>>> method_numeric_params{};
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> method_string_params{};
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> method_ipv4_params{};

    for (const auto& [_, item] : local_queue) {
        if (!item) {
            continue;
        }

        const std::string method = item->get_method_type().empty() ? "sensor-data" : item->get_method_type();
        const std::string param_name = item->get_param_name();
        const std::string param_value = item->get_param_value();
        const std::string value_key = build_value_key(*item);

        if (method.empty() || param_name.empty()) {
            continue;
        }

        if (method_should_be_sent.find(method) == method_should_be_sent.end()) {
            method_should_be_sent[method] = false;
        }

        if (method == "sensor-error") {
            // `sensor-error` is string-only and should only be sent when changed.
            const bool was_sent = g_last_sent_string_values.find(value_key) != g_last_sent_string_values.end();
            if (!was_sent || g_last_sent_string_values[value_key] != param_value) {
                method_string_params[method].emplace_back(param_name, param_value);
                method_should_be_sent[method] = true;
            }
            continue;
        }

        if (param_name == "IPv4") {
            // Historically emitted as a raw value field, not as a quoted string object.
            method_ipv4_params[method].emplace_back(param_name, param_value);
            method_should_be_sent[method] = true;
            continue;
        }

        if (is_numeric_string(param_value)) {
            const float new_value = std::strtof(param_value.c_str(), nullptr);
            method_numeric_params[method].emplace_back(param_name, new_value);
            g_last_sent_string_values.erase(value_key);

            const bool was_sent = g_last_sent_values.find(value_key) != g_last_sent_values.end();
            if (was_sent) {
                const float last_value = g_last_sent_values[value_key];
                if (should_send_numeric(param_name, last_value, new_value)) {
                    method_should_be_sent[method] = true;
                }
            } else {
                method_should_be_sent[method] = true;
            }
        } else {
            method_string_params[method].emplace_back(param_name, param_value);
            method_should_be_sent[method] = true;
        }
    }

    for (auto& [method, should_send] : method_should_be_sent) {
        if (!should_send) {
            continue;
        }

        std::string json_message = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\"";
        if (has_valid_time()) {
            json_message += ",\"timestamp\":" + std::to_string(unix_timestamp());
        }
        json_message += ",\"params\":{";
        bool first_param = true;

        if (method_numeric_params.count(method)) {
            for (const auto& [name, value] : method_numeric_params[method]) {
                if (!first_param) {
                    json_message += ",";
                }
                char buf[96];
                std::snprintf(buf, sizeof(buf), "\"%s\":{\"value\":%.2f}", name.c_str(), value);
                json_message += buf;
                g_last_sent_values[name] = value;
                first_param = false;
            }
        }

        if (method_ipv4_params.count(method)) {
            for (const auto& [name, value] : method_ipv4_params[method]) {
                if (!first_param) {
                    json_message += ",";
                }
                json_message += "\"" + name + "\":{\"value\":" + value + "}";
                first_param = false;
            }
        }

        if (method_string_params.count(method)) {
            for (const auto& [name, value] : method_string_params[method]) {
                if (!first_param) {
                    json_message += ",";
                }
                json_message += "\"" + name + "\":{\"value\":\"" + value + "\"}";
                if (method == "sensor-error") {
                    g_last_sent_string_values[name] = value;
                }
                first_param = false;
            }
        }

        json_message += "}}";

        const std::string topic = build_stream_topic("rpcout");
        mqtt_enqueue_message(topic, json_message, 0, false);
        ESP_LOGI(TAG, "Published dataQueue method=%s topic=%s payload=%s",
                 method.c_str(),
                 topic.c_str(),
                 json_message.c_str());

        should_send = false;
    }
}

// Dedicated worker that periodically attempts to drain the logical queue.
void data_queue_task(void*) {
    for (;;) {
        process_data_queue_once();
        vTaskDelay(pdMS_TO_TICKS(kDataQueueProcessPeriodMs));
    }
}

}  // namespace

GenericDataItem::GenericDataItem(std::string param_name,
                                 std::string param_value,
                                 std::string method_type,
                                 std::string device_id)
    : param_name_(std::move(param_name)),
      param_value_(std::move(param_value)),
      method_type_(std::move(method_type)),
      device_id_(std::move(device_id)) {}

std::string GenericDataItem::get_param_name() const {
    return param_name_;
}

std::string GenericDataItem::get_param_value() const {
    return param_value_;
}

std::string GenericDataItem::get_method_type() const {
    return method_type_;
}

std::string GenericDataItem::get_device_id() const {
    return device_id_;
}

void data_queue_upsert(std::unique_ptr<DataItem> item) {
    if (!item) {
        return;
    }

    // Replacing by key keeps memory bounded and always preserves the newest sample.
    const std::string key = build_item_key(*item);
    std::lock_guard<std::mutex> lock(g_data_queue_mutex);
    g_data_queue[key] = std::move(item);
}

void data_queue_upsert_generic(std::string param_name,
                               std::string param_value,
                               std::string method_type,
                               std::string device_id) {
    data_queue_upsert(std::make_unique<GenericDataItem>(
        std::move(param_name),
        std::move(param_value),
        std::move(method_type),
        std::move(device_id)));
}

size_t data_queue_size() {
    std::lock_guard<std::mutex> lock(g_data_queue_mutex);
    return g_data_queue.size();
}

void data_queue_start_task() {
    xTaskCreate(data_queue_task, "core_data_queue", kDataQueueTaskStack, nullptr, 3, nullptr);
}

}  // namespace core
