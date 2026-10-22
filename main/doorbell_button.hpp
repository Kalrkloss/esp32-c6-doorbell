// SPDX-License-Identifier: MIT
// Physical doorbell button: an LP/RTC capable GPIO that wakes the chip from deep sleep.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace doorbell {

// Configures the GPIO as input with pull-up (also in the RTC domain, so the line
// stays defined while the digital domain is powered down).
esp_err_t button_init();

// Current contact state: true = closed = pressed.
bool button_is_pressed();

// Waits until the contact opens. Returns false if it is still closed after
// timeout_ms.
bool button_wait_release(uint32_t timeout_ms);

}  // namespace doorbell
