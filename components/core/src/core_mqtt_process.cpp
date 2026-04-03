#include "core_mqtt_process.h"

#include "core.h"
#include "core_internal.h"

#include <array>
#include <cstdlib>
#include <cstring>

#include <cJSON.h>
#include <esp_log.h>

namespace core {

namespace {

static const char* TAG = "core_mqtt_proc";

enum class RouteMatchKind : uint8_t {
    Exact = 0,
    Prefix,
};

enum class TopicFamily : uint8_t {
    Command = 0,
    Rpc,
};

enum class RouteCommand : uint8_t {
    Upgrade = 0,
    Reboot,
    Reset,
    TargetTemp,
    RpcDefault,
};

struct RouteEntry {
    const char* suffix;
    int qos;
    TopicFamily topic_family;
    RouteMatchKind match_kind;
    RouteCommand command;
};

// Single contributor-owned route table.
//
// To add a new command from the platform, extend this table and then add the
// handling branch below in `handle_core_mqtt_route`.
inline constexpr std::array<RouteEntry, 5> kRoutes{{
    {"upgrade", 1, TopicFamily::Command, RouteMatchKind::Exact, RouteCommand::Upgrade},
    {"reboot", 1, TopicFamily::Command, RouteMatchKind::Exact, RouteCommand::Reboot},
    {"reset", 1, TopicFamily::Command, RouteMatchKind::Exact, RouteCommand::Reset},
    {"set-temp", 1, TopicFamily::Command, RouteMatchKind::Exact, RouteCommand::TargetTemp},
    {"default", 1, TopicFamily::Rpc, RouteMatchKind::Exact, RouteCommand::RpcDefault},
}};

std::string build_rpc_topic(const char* suffix) {
    return "rpc/" + chip_id() + "/" + suffix;
}

bool parse_numeric_payload_value(const std::string& payload, float& out_value) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root) {
        const cJSON* value_node = root;
        if (cJSON_IsObject(root)) {
            value_node = cJSON_GetObjectItem(root, "value");
        }
        if (value_node && cJSON_IsNumber(value_node)) {
            out_value = static_cast<float>(value_node->valuedouble);
            cJSON_Delete(root);
            return true;
        }
        cJSON_Delete(root);
    }

    char* end = nullptr;
    const float parsed = std::strtof(payload.c_str(), &end);
    if (end && *end == '\0') {
        out_value = parsed;
        return true;
    }
    return false;
}

bool parse_numeric_json_value(const cJSON* node, float& out_value) {
    if (!node) {
        return false;
    }
    if (cJSON_IsNumber(node)) {
        out_value = static_cast<float>(node->valuedouble);
        return true;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        char* end = nullptr;
        const float parsed = std::strtof(node->valuestring, &end);
        if (end && *end == '\0') {
            out_value = parsed;
            return true;
        }
    }
    return false;
}

bool parse_rpc_upgrade_url(const cJSON* args, std::string& out_url) {
    if (!args) {
        return false;
    }
    if (cJSON_IsString(args) && args->valuestring) {
        out_url = args->valuestring;
        return !out_url.empty();
    }
    if (!cJSON_IsObject(args)) {
        return false;
    }

    static constexpr const char* kUrlKeys[] = {"url", "value", "uri"};
    for (const char* key : kUrlKeys) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(args, key);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
            out_url = item->valuestring;
            return true;
        }
    }
    return false;
}

bool handle_rpc_payload(const std::string& topic, const std::string& payload) {
    ESP_LOGI(TAG, "MQTT rpc recv topic=%s payload=%s", topic.c_str(), payload.c_str());

    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        ESP_LOGW(TAG, "MQTT rpc JSON parse failed topic=%s payload=%s", topic.c_str(), payload.c_str());
        return true;
    }

    char* pretty = cJSON_Print(root);
    if (pretty) {
        ESP_LOGI(TAG, "MQTT rpc JSON topic=%s\n%s", topic.c_str(), pretty);
        cJSON_free(pretty);
    }

    const cJSON* method_node = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON* id_node = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON* key = params ? cJSON_GetObjectItemCaseSensitive(params, "key") : nullptr;
    const cJSON* args = params ? cJSON_GetObjectItemCaseSensitive(params, "args") : nullptr;
    const cJSON* command_type_node = (key && cJSON_IsArray(key)) ? cJSON_GetArrayItem(key, 0) : nullptr;
    const cJSON* target_node = (key && cJSON_IsArray(key)) ? cJSON_GetArrayItem(key, 1) : nullptr;

    const char* method = cJSON_IsString(method_node) ? method_node->valuestring : "";
    const char* command_type = cJSON_IsString(command_type_node) ? command_type_node->valuestring : "";
    const char* target = cJSON_IsString(target_node) ? target_node->valuestring : "";
    const double rpc_id = cJSON_IsNumber(id_node) ? id_node->valuedouble : -1.0;

    ESP_LOGI(TAG,
             "MQTT rpc parsed topic=%s method=%s id=%.0f command=%s target=%s",
             topic.c_str(),
             method,
             rpc_id,
             command_type,
             target);

    if (command_type[0] == '\0') {
        ESP_LOGW(TAG, "MQTT rpc missing params.key[0] topic=%s", topic.c_str());
        cJSON_Delete(root);
        return true;
    }

    if (strcmp(method, "put") != 0) {
        ESP_LOGW(TAG,
                 "MQTT rpc unsupported method topic=%s method=%s command=%s",
                 topic.c_str(),
                 method,
                 command_type);
        cJSON_Delete(root);
        return true;
    }

    if (strcmp(command_type, "upgrade") == 0) {
        std::string url;
        if (!parse_rpc_upgrade_url(args, url)) {
            ESP_LOGW(TAG, "MQTT rpc upgrade missing URL topic=%s", topic.c_str());
            cJSON_Delete(root);
            return true;
        }
        cJSON_Delete(root);
        if (perform_ota_update(url.c_str()) == ESP_OK) {
            safe_restart();
        }
        return true;
    }

    if (strcmp(command_type, "reboot") == 0) {
        cJSON_Delete(root);
        safe_restart();
        return true;
    }

    if (strcmp(command_type, "reset") == 0) {
        cJSON_Delete(root);
        clear_runtime_credentials();
        safe_restart();
        return true;
    }

    if (strncmp(command_type, "target-temp", 11) == 0) {
        float value = 0.0f;
        if (!parse_numeric_json_value(args, value)) {
            ESP_LOGW(TAG,
                     "MQTT rpc target-temp failed to parse args topic=%s command=%s",
                     topic.c_str(),
                     command_type);
            cJSON_Delete(root);
            return true;
        }
        ESP_LOGI(TAG,
                 "MQTT rpc target-temp accepted topic=%s command=%s target=%s value=%.2f",
                 topic.c_str(),
                 command_type,
                 target,
                 static_cast<double>(value));
        cJSON_Delete(root);
        return true;
    }

    ESP_LOGW(TAG,
             "MQTT rpc command has no mapped handler topic=%s method=%s command=%s target=%s",
             topic.c_str(),
             method,
             command_type,
             target);
    cJSON_Delete(root);
    return true;
}

std::string build_route_topic(const RouteEntry& route) {
    if (route.topic_family == TopicFamily::Rpc) {
        return build_rpc_topic(route.suffix);
    }
    return build_command_topic(route.suffix);
}

const RouteEntry* match_route(const std::string& topic, std::string& topic_tail) {
    for (const RouteEntry& route : kRoutes) {
        const std::string base_topic = build_route_topic(route);
        if (route.match_kind == RouteMatchKind::Exact) {
            if (topic == base_topic) {
                topic_tail.clear();
                return &route;
            }
            continue;
        }

        if (topic == base_topic) {
            topic_tail.clear();
            return &route;
        }

        const std::string topic_prefix = base_topic + "/";
        if (topic.rfind(topic_prefix, 0) == 0) {
            topic_tail = topic.substr(topic_prefix.size());
            return &route;
        }
    }
    return nullptr;
}

}  // namespace

void subscribe_core_mqtt_routes(esp_mqtt_client_handle_t client, const std::string& broker_uri) {
    for (const RouteEntry& route : kRoutes) {
        const std::string topic = build_route_topic(route);
        const int msg_id = esp_mqtt_client_subscribe(client, topic.c_str(), route.qos);
        ESP_LOGI(TAG,
                 "MQTT subscribe endpoint=%s topic=%s qos=%d msg_id=%d",
                 broker_uri.c_str(),
                 topic.c_str(),
                 route.qos,
                 msg_id);
    }
}

bool handle_core_mqtt_route(const std::string& topic, const std::string& payload) {
    std::string topic_tail;
    const RouteEntry* route = match_route(topic, topic_tail);
    if (!route) {
        return false;
    }

    if (!topic_tail.empty()) {
        ESP_LOGI(TAG, "MQTT recv topic tail=%s", topic_tail.c_str());
    }

    switch (route->command) {
        case RouteCommand::Upgrade:
            if (perform_ota_update(payload.c_str()) == ESP_OK) {
                safe_restart();
            }
            return true;
        case RouteCommand::Reboot:
            safe_restart();
            return true;
        case RouteCommand::Reset:
            clear_runtime_credentials();
            safe_restart();
            return true;
        case RouteCommand::TargetTemp: {
            // Platform message examples for contributors:
            //   topic:   command/<chip_id>/target-temp
            //   payload: 55
            //   payload: {"value":55}
            float value = 0.0f;
            if (!parse_numeric_payload_value(payload, value)) {
                ESP_LOGW(TAG,
                         "MQTT target-temp failed to parse payload topic=%s payload=%s",
                         topic.c_str(),
                         payload.c_str());
                return true;
            }

            // Replace this example action with application-specific behavior.
            ESP_LOGI(TAG,
                     "MQTT target-temp accepted topic=%s value=%.2f",
                     topic.c_str(),
                     static_cast<double>(value));
            app_handle_set_temp(value);
            return true;
        }
        case RouteCommand::RpcDefault:
            return handle_rpc_payload(topic, payload);
    }

    return false;
}

}  // namespace core
