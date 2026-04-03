#pragma once

#include "core_data_queue.h"
#include <stdint.h>
#include "core_build_info.h"

namespace core {

// Hardware pins owned by the application entrypoint.
// The core uses these values to attach the local button and LED indication,
// but the application remains responsible for selecting the board mapping.
struct CoreHardwareConfig {
    int button_pin{CORE_BUTTON_PIN};
    int status_led_pin{CORE_STATUS_LED_PIN};
    int activity_led_pin{CORE_ACTIVITY_LED_PIN};
};

// Initializes the shared runtime core.
//
// This is the single public entrypoint used by the firmware application.
// The call wires together:
// - NVS-backed runtime configuration
// - button and LED local UX
// - Wi-Fi stack and provisioning transport
// - MQTT runtime and telemetry buffering
// - background maintenance for time sync and remote config refresh
void init(const CoreHardwareConfig& hardware);

// Starts background MQTT-related workers.
//
// This remains public because the application entrypoint may choose to split
// initialization sequencing in the future, but current firmware calls it from
// `core::init()` and does not need to call it directly.
void mqtt_start_tasks();

// Application-owned hook for the incoming `command/<chip_id>/set-temp` route.
//
// The core parses the MQTT payload into a float and then calls this hook.
// Firmware applications can override it in `main.cpp` to bridge the command
// into local business logic or telemetry without modifying the core.
void app_handle_set_temp(float value);

}  // namespace core
