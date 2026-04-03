#include "core_internal.h"
#include "core_runtime_constants.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace core {

namespace {

static const char* TAG = "core_button";
constexpr ledc_mode_t kLedSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kLedTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kStatusLedChannel = LEDC_CHANNEL_0;
constexpr ledc_channel_t kActivityLedChannel = LEDC_CHANNEL_1;

// Small stateless helpers used by LED pattern generation.
bool blink_ms(uint32_t now_ms, uint32_t period_ms) {
    if (period_ms == 0) {
        return false;
    }
    return ((now_ms / period_ms) % 2U) == 0U;
}

bool pulse_window_ms(uint32_t now_ms, uint32_t cycle_ms, uint32_t on_ms) {
    if (cycle_ms == 0) {
        return false;
    }
    return (now_ms % cycle_ms) < on_ms;
}

// Produces a soft "breathing" envelope for single-color LEDs.
//
// We use a triangle wave with a quadratic ease so the LED spends more time near
// the low end and ramps smoothly through the visible part of the cycle.
uint32_t breathe_duty(uint32_t now_ms) {
    const uint32_t phase = now_ms % kLedBreathCycleMs;
    const float normalized = (phase < (kLedBreathCycleMs / 2U))
                                 ? static_cast<float>(phase) / static_cast<float>(kLedBreathCycleMs / 2U)
                                 : static_cast<float>(kLedBreathCycleMs - phase) / static_cast<float>(kLedBreathCycleMs / 2U);
    const float eased = normalized * normalized;
    return static_cast<uint32_t>(eased * static_cast<float>(kLedDutyMax));
}

void write_led_duty(int pin, ledc_channel_t channel, uint32_t duty) {
    if (pin < 0) {
        return;
    }
    ledc_set_duty(kLedSpeedMode, channel, duty);
    ledc_update_duty(kLedSpeedMode, channel);
}

void configure_led_channel(int pin, ledc_channel_t channel) {
    if (pin < 0) {
        return;
    }

    ledc_channel_config_t led = {};
    led.gpio_num = pin;
    led.speed_mode = kLedSpeedMode;
    led.channel = channel;
    led.intr_type = LEDC_INTR_DISABLE;
    led.timer_sel = kLedTimer;
    led.duty = 0;
    led.hpoint = 0;
    ledc_channel_config(&led);
}

// LED renderer.
//
// `Auto` derives indication from current connectivity/auth state.
// Forced modes override the derived behavior for explicit UX moments such as
// provisioning indication or button-hold feedback.
void led_task(void*) {
    RuntimeState& s = state();
    for (;;) {
        const uint32_t now_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t status_duty = 0;
        uint32_t activity_duty = 0;

        const LedMode forced_mode = s.led_mode.load();
        RuntimeConfig cfg{};
        {
            std::lock_guard<std::mutex> lock(s.config_mutex);
            cfg = s.config;
        }

        if (forced_mode == LedMode::ButtonHold) {
            status_duty = blink_ms(now_ms, kLedSlowBlinkMs) ? kLedDutyMax : 0;
            activity_duty = status_duty;
        } else if (cfg.wifi_ssid.empty()) {
            // No stored Wi-Fi credentials: both LEDs blink fast.
            status_duty = blink_ms(now_ms, kLedFastBlinkMs) ? kLedDutyMax : 0;
            activity_duty = status_duty;
        } else if (!s.wifi_connected) {
            // Wi-Fi credentials exist, but STA link is down: slow status blink only.
            status_duty = blink_ms(now_ms, kLedSlowBlinkMs) ? kLedDutyMax : 0;
            activity_duty = 0;
        } else if (cfg.access_token.empty()) {
            // Wi-Fi is up, but cloud auth is incomplete: gentle status pulse.
            status_duty = pulse_window_ms(now_ms, kLedPulseCycleMs, kLedPulseOnMs) ? kLedDutyMax : 0;
            activity_duty = 0;
        } else if (!s.mqtt_connected) {
            // Token exists, MQTT not up yet: status pulse + activity slow blink.
            status_duty = pulse_window_ms(now_ms, kLedPulseCycleMs, kLedPulseOnMs) ? kLedDutyMax : 0;
            activity_duty = blink_ms(now_ms, kLedSlowBlinkMs) ? kLedDutyMax : 0;
        } else {
            // Fully online: both LEDs "breathe" so the device looks alive
            // without reading like an alert or transitional state.
            status_duty = breathe_duty(now_ms);
            activity_duty = (s.activity_led_pin >= 0) ? status_duty : 0;
        }

        switch (forced_mode) {
            case LedMode::Off:
                status_duty = 0;
                activity_duty = 0;
                break;
            case LedMode::Provisioning:
                status_duty = blink_ms(now_ms, kLedFastBlinkMs) ? kLedDutyMax : 0;
                activity_duty = status_duty;
                break;
            case LedMode::ConnectingWifi:
                status_duty = blink_ms(now_ms, kLedSlowBlinkMs) ? kLedDutyMax : 0;
                activity_duty = 0;
                break;
            case LedMode::WaitingToken:
                status_duty = pulse_window_ms(now_ms, kLedPulseCycleMs, kLedPulseOnMs) ? kLedDutyMax : 0;
                activity_duty = 0;
                break;
            case LedMode::ConnectingMqtt:
                status_duty = pulse_window_ms(now_ms, kLedPulseCycleMs, kLedPulseOnMs) ? kLedDutyMax : 0;
                activity_duty = blink_ms(now_ms, kLedSlowBlinkMs) ? kLedDutyMax : 0;
                break;
            case LedMode::Online:
                status_duty = breathe_duty(now_ms);
                activity_duty = (s.activity_led_pin >= 0) ? status_duty : 0;
                break;
            case LedMode::Error:
                status_duty = blink_ms(now_ms, kLedFastBlinkMs) ? kLedDutyMax : 0;
                activity_duty = status_duty;
                break;
            case LedMode::Auto:
            case LedMode::ButtonHold:
            break;
        }

        write_led_duty(s.status_led_pin, kStatusLedChannel, status_duty);
        write_led_duty(s.activity_led_pin, kActivityLedChannel, activity_duty);
        vTaskDelay(pdMS_TO_TICKS(kLedTaskStepMs));
    }
}

// Local button workflow.
//
// Current contract:
// - hold feedback starts quickly so the user sees the button was accepted
// - reset is evaluated on release, not while still pressed
// - >=10s release clears all provisioning/runtime credentials and reboots
void button_task(void*) {
    RuntimeState& s = state();
    bool last_pressed = false;
    bool hold_feedback_armed = false;
    int64_t press_started_us = 0;

    for (;;) {
        const bool pressed = gpio_get_level(static_cast<gpio_num_t>(s.button_pin)) == 0;

        if (pressed) {
            if (!last_pressed) {
                press_started_us = esp_timer_get_time();
                ESP_LOGI(TAG, "button pressed on GPIO%d", s.button_pin);
            }

            const uint32_t held_ms = static_cast<uint32_t>((esp_timer_get_time() - press_started_us) / 1000ULL);
            if (held_ms >= kButtonHoldFeedbackMs && !hold_feedback_armed) {
                s.led_mode = LedMode::ButtonHold;
                hold_feedback_armed = true;
                ESP_LOGI(TAG, "button hold feedback armed");
            }
        } else if (last_pressed) {
            const uint32_t held_ms = static_cast<uint32_t>((esp_timer_get_time() - press_started_us) / 1000ULL);
            ESP_LOGI(TAG, "button released after %u ms", static_cast<unsigned>(held_ms));
            s.led_mode = LedMode::Auto;
            hold_feedback_armed = false;
            press_started_us = 0;

            if (held_ms >= kButtonFactoryResetMs) {
                ESP_LOGW(TAG, "button released after %u ms -> clearing provisioning data and rebooting",
                         static_cast<unsigned>(held_ms));
                clear_runtime_credentials();
                safe_restart();
            }
        }

        last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(kButtonPollPeriodMs));
    }
}

}  // namespace

void init_button_and_leds() {
    // GPIO ownership for button/LEDs is local to the core after init.
    //
    // We keep the button on plain GPIO input, but move LEDs to LEDC so the
    // online state can use a true breathing envelope instead of binary flashing.
    RuntimeState& s = state();

    gpio_config_t button = {};
    button.pin_bit_mask = (1ULL << s.button_pin);
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&button);

    ESP_LOGI(TAG, "button task configured on GPIO%d (active low)", s.button_pin);

    ledc_timer_config_t timer = {};
    timer.speed_mode = kLedSpeedMode;
    timer.timer_num = kLedTimer;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    configure_led_channel(s.status_led_pin, kStatusLedChannel);
    configure_led_channel(s.activity_led_pin, kActivityLedChannel);

    xTaskCreate(button_task, "core_button", kButtonTaskStack, nullptr, 4, nullptr);
    xTaskCreate(led_task, "core_led", kLedTaskStack, nullptr, 2, nullptr);
}

}  // namespace core
