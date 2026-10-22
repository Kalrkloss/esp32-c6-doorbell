// SPDX-License-Identifier: MIT
// Zigbee side: sleepy end device that exposes a doorbell push button.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace doorbell {

// Starts the Zigbee stack task (init, endpoint registration, commissioning).
esp_err_t zigbee_start();

// True once the device is attached to a network (fresh join or re-attach after wake).
bool zigbee_joined();

// True if the device just joined a network for the first time; the coordinator
// still has to interview it, so the caller should stay awake for a while.
bool zigbee_fresh_join();

// Blocks until the device is attached to its network (or timeout_ms elapsed).
bool zigbee_wait_ready(uint32_t timeout_ms);

// Reports the button state through the Multistate Input cluster:
//   1 -> contact closed (press), 0 -> contact open (release).
// Blocks until the coordinator confirms (or kReportTimeoutMs elapsed).
esp_err_t report_button_state(uint16_t state);

}  // namespace doorbell
