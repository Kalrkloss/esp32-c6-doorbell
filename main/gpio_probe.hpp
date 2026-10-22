// SPDX-License-Identifier: MIT
// Bring-up helper: log the levels of the LP-capable GPIOs.

#pragma once

namespace doorbell {

// Starts a task that prints GPIO0..GPIO10 levels every 500 ms.
void gpio_probe_start();

}  // namespace doorbell
