#pragma once

// Shared firmware identity and default hardware pins.
//
// This header is intentionally simple and preprocessor-based so the same values
// can be overridden from either PlatformIO, native ESP-IDF, or a future build
// system without touching the core implementation.
//
// Contributors should treat these macros as build-time identity inputs, not as
// runtime configuration. Values that may change in the field belong in NVS and
// are handled by the provisioning/runtime code instead.

#ifndef CORE_VENDOR_CODE
#define CORE_VENDOR_CODE "vendor"
#endif

#ifndef CORE_MODEL_CODE
#define CORE_MODEL_CODE "mydevice"
#endif

#ifndef CORE_FW_VERSION
#define CORE_FW_VERSION "0.01"
#endif

#ifndef CORE_HW_CODE
#define CORE_HW_CODE nullptr
#endif

#ifndef CORE_BUTTON_PIN
#define CORE_BUTTON_PIN 0
#endif

#ifndef CORE_STATUS_LED_PIN
#define CORE_STATUS_LED_PIN 2
#endif

#ifndef CORE_ACTIVITY_LED_PIN
#define CORE_ACTIVITY_LED_PIN -1
#endif
