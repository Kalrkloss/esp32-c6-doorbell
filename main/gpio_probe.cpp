// SPDX-License-Identifier: MIT
// Bring-up helper: log the levels of the LP-capable GPIOs so the pin a button is
// actually wired to can be identified. Only compiled with CONFIG_DOORBELL_GPIO_PROBE.

#include "gpio_probe.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace doorbell {
namespace {

constexpr const char *TAG = "PROBE";

// ESP32-C6: GPIO0..GPIO7 are the LP/RTC capable pins (usable as deep-sleep
// wake-up source). GPIO8/9 are probed as well - they are common BOOT pins.
constexpr int kFirstPin = 0;
constexpr int kLastPin = 10;

void probe_task(void *)
{
    for (int pin = kFirstPin; pin <= kLastPin; pin++) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << pin;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io);
    }

    ESP_LOGW(TAG, "probing GPIO%d..GPIO%d, press the button now", kFirstPin, kLastPin);

    for (;;) {
        char line[160] = {0};
        int offset = 0;
        for (int pin = kFirstPin; pin <= kLastPin; pin++) {
            offset += snprintf(line + offset, sizeof(line) - offset, "%d=%d ", pin, gpio_get_level(static_cast<gpio_num_t>(pin)));
        }
        ESP_LOGI(TAG, "%s", line);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

void gpio_probe_start()
{
    xTaskCreate(probe_task, "gpio_probe", 3072, nullptr, 3, nullptr);
}

}  // namespace doorbell
