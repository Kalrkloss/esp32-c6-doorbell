// SPDX-License-Identifier: MIT
// ESP32-C6 Zigbee doorbell button - application entry point.
//
// Deep-sleep design: the device is powered down to ~7 uA and wakes on the
// doorbell contact (GPIO, LP/RTC domain). Every wake-up is a cold boot, so the
// sequence is:
//
//   boot -> NVS -> stack restores its network state and re-attaches ->
//   report the press (Multistate Input PresentValue 1 / 0) -> deep sleep
//
// A press that cannot be delivered (no network) is kept in RTC memory and
// retried on the next wake-up.

#include <atomic>
#include <cinttypes>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doorbell_button.hpp"
#include "doorbell_config.hpp"
#if CONFIG_DOORBELL_GPIO_PROBE
#include "gpio_probe.hpp"
#endif
#include "doorbell_device.hpp"

namespace {

constexpr const char *TAG = "APP";

// Survives deep sleep (RTC domain keeps its content).
RTC_DATA_ATTR uint32_t s_boot_count = 0;
RTC_DATA_ATTR uint32_t s_pending_presses = 0;

bool woke_on_button() { return (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1)) != 0; }

// A wake-up from deep sleep always reports one of the sleep wake sources. Any
// other reset (power-on, EN, USB) leaves ESP_SLEEP_WAKEUP_UNDEFINED set, which
// is what opens the flashing/serial window.
bool woke_from_deep_sleep()
{
    constexpr uint32_t kSleepSources = BIT(ESP_SLEEP_WAKEUP_EXT0) | BIT(ESP_SLEEP_WAKEUP_EXT1) |
                                       BIT(ESP_SLEEP_WAKEUP_TIMER) | BIT(ESP_SLEEP_WAKEUP_GPIO) |
                                       BIT(ESP_SLEEP_WAKEUP_ULP);
    return (esp_sleep_get_wakeup_causes() & kSleepSources) != 0;
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    return err;
}

// Deep sleep until the contact closes again (and/or the timer fires).
[[noreturn]] void go_to_sleep(uint32_t timer_seconds)
{
    // A contact that is still closed would wake the chip again immediately, so
    // in that case the wake-up is left to the timer.
    const bool released = doorbell::button_wait_release(doorbell::kMaxPressDurationMs);

    if (released) {
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << doorbell::kButtonGpio, ESP_EXT1_WAKEUP_ANY_LOW));
    } else {
        ESP_LOGW(TAG, "contact still closed, waking on timer only");
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    }

    const uint32_t seconds = released ? timer_seconds : (timer_seconds > 0 ? timer_seconds : doorbell::kRetrySleepS);
    if (seconds > 0) {
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL));
        ESP_LOGI(TAG, "deep sleep, next wake-up in %" PRIu32 " s or on button", seconds);
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        ESP_LOGI(TAG, "deep sleep until button press");
    }

    esp_deep_sleep_start();
}

// Sends press + release for a queued press. On failure the press stays queued.
void deliver_pending_press()
{
    if (s_pending_presses == 0) {
        return;
    }

    ESP_LOGI(TAG, "delivering %" PRIu32 " queued press(es)", s_pending_presses);
    if (doorbell::report_button_state(1) != ESP_OK) {
        ESP_LOGW(TAG, "press not delivered, keeping it for the next wake-up");
        return;
    }
    doorbell::button_wait_release(doorbell::kMaxPressDurationMs);
    doorbell::report_button_state(0);

    s_pending_presses = 0;
    vTaskDelay(pdMS_TO_TICKS(doorbell::kDwellAfterReportMs));
}

// Presses during the interview window are reported right away.
void handle_press_while_awake()
{
    if (!doorbell::button_is_pressed()) {
        return;
    }
    ESP_LOGI(TAG, "press while awake");
    if (doorbell::report_button_state(1) != ESP_OK) {
        return;
    }
    doorbell::button_wait_release(doorbell::kMaxPressDurationMs);
    doorbell::report_button_state(0);
    vTaskDelay(pdMS_TO_TICKS(doorbell::kDwellAfterReportMs));
}

// After the first join the coordinator still has to interview the device; going
// back to sleep now would stall it. Stay awake and keep polling.
void stay_awake_for_interview()
{
    ESP_LOGW(TAG, "fresh join: staying awake %d s so the coordinator can interview us",
             CONFIG_DOORBELL_JOIN_AWAKE_S);
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(doorbell::kJoinAwakeMs);
    while (xTaskGetTickCount() < end) {
        handle_press_while_awake();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

extern "C" void app_main(void)
{
    const esp_err_t nvs_err = init_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));
    }
    ESP_ERROR_CHECK(doorbell::button_init());

    s_boot_count++;
    const bool button_wake = woke_on_button();
#if CONFIG_DOORBELL_SELFTEST
    const bool press = true;
#else
    const bool press = button_wake;
#endif
    if (press && s_pending_presses < doorbell::kMaxPendingPresses) {
        s_pending_presses++;
    }

    const bool power_on = !woke_from_deep_sleep();
    ESP_LOGI(TAG, "boot #%" PRIu32 " (wake cause 0x%" PRIx32 ": %s), %" PRIu32 " press(es) queued, contact %s",
             s_boot_count, esp_sleep_get_wakeup_causes(),
             button_wake ? "button" : (power_on ? "power-on/reset" : "timer"), s_pending_presses,
             doorbell::button_is_pressed() ? "closed" : "open");

#if CONFIG_DOORBELL_GPIO_PROBE
    doorbell::gpio_probe_start();
#endif

    ESP_ERROR_CHECK(doorbell::zigbee_start());

    if (!doorbell::zigbee_wait_ready(doorbell::kNetworkWaitMs)) {
        ESP_LOGW(TAG, "no network within %" PRIu32 " ms, sleeping %" PRIu32 " s and retrying",
                 doorbell::kNetworkWaitMs, doorbell::kRetrySleepS);
        go_to_sleep(doorbell::kRetrySleepS);
    }

    if (doorbell::zigbee_fresh_join()) {
        stay_awake_for_interview();
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    deliver_pending_press();

    // Deep sleep powers the USB PHY down, i.e. the board vanishes from
    // /dev/ttyACM* while asleep. A power-on boot therefore holds the port open
    // for a while: that is the window used for flashing and the serial console.
    if (power_on && doorbell::kBootAwakeMs > 0) {
        ESP_LOGW(TAG, "power-on: keeping the serial/USB window open for %d s", CONFIG_DOORBELL_BOOT_AWAKE_S);
        const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(doorbell::kBootAwakeMs);
        while (xTaskGetTickCount() < end) {
            handle_press_while_awake();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

#if CONFIG_DOORBELL_SELFTEST
    // Bench test: every boot simulates one press, then the normal deep sleep
    // path is taken (with a short timer so the cycle repeats by itself).
    go_to_sleep(CONFIG_DOORBELL_SELFTEST_REBOOT_S);
#else
    go_to_sleep(doorbell::kKeepaliveWakeS);
#endif
}
