#pragma once

#include <mqtt_client.h>
#include <string>

namespace core {

// Contributor-facing MQTT process layer.
//
// This file intentionally mirrors the old `mqttProcess` idea:
// - subscriptions are declared in one place
// - topic matching lives рядом with command logic
// - payload parsing examples live next to the route that uses them
//
// `core_mqtt.cpp` should stay transport-only and delegate all route-specific
// behavior here.
void subscribe_core_mqtt_routes(esp_mqtt_client_handle_t client, const std::string& broker_uri);
bool handle_core_mqtt_route(const std::string& topic, const std::string& payload);

}  // namespace core
