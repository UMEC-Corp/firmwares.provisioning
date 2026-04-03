#include "core.h"

#include <cstdio>

#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr TickType_t kRssiPublishPeriod = pdMS_TO_TICKS(10000);

void wifi_rssi_task(void*) {
    for (;;) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            core::data_queue_upsert_generic("wifi", std::to_string(rssi), "sensor-data");
        }
        vTaskDelay(kRssiPublishPeriod);
    }
}

}  // namespace

namespace core {

void app_handle_set_temp(float value) {
    char value_buffer[32] = {};
    std::snprintf(value_buffer, sizeof(value_buffer), "%.2f", static_cast<double>(value));
    data_queue_upsert_generic("temp", value_buffer, "sensor-data");
}

}  // namespace core

extern "C" void app_main(void) {
    core::CoreHardwareConfig hardware{};
    hardware.button_pin = CORE_BUTTON_PIN;
    hardware.status_led_pin = CORE_STATUS_LED_PIN;
    hardware.activity_led_pin = CORE_ACTIVITY_LED_PIN;
    core::init(hardware);
    xTaskCreate(wifi_rssi_task, "app_wifi_rssi", 3072, nullptr, 3, nullptr);
}
