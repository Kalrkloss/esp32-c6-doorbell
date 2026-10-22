// SPDX-License-Identifier: MIT
// Physical doorbell button: an LP/RTC capable GPIO that wakes the chip from deep sleep.

#include "doorbell_button.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include "driver/rtc_io.h"
#endif

#include "doorbell_config.hpp"

namespace doorbell {
namespace {

constexpr const char *TAG = "BTN";

}  // namespace

esp_err_t button_init()
{
    const auto gpio = static_cast<gpio_num_t>(kButtonGpio);

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << gpio;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
    // Keep the pull-up alive in the RTC domain, otherwise the contact line
    // floats while the digital domain is powered down and the wake-up source
    // would trigger spuriously.
    rtc_gpio_pulldown_dis(gpio);
    rtc_gpio_pullup_en(gpio);
#endif

    ESP_LOGI(TAG, "GPIO%d as doorbell contact (pull-up, active low)", kButtonGpio);
    return ESP_OK;
}

bool button_is_pressed() { return gpio_get_level(static_cast<gpio_num_t>(kButtonGpio)) == 0; }

bool button_wait_release(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (button_is_pressed()) {
        if (xTaskGetTickCount() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

}  // namespace doorbell
