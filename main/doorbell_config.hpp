// SPDX-License-Identifier: MIT
// Compile-time configuration of the doorbell firmware.

#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace doorbell {

// Endpoint that carries the whole doorbell button (basic/identify/multistate input).
constexpr uint8_t kEndpointId = 1;

// ZCL character strings are length prefixed: 0x03 "DIY", 0x0b "C6-DOORBELL".
// The model identifier is what the Zigbee2MQTT external converter matches on.
constexpr char kManufacturerName[] = "\x03"
                                     "DIY";
constexpr char kModelIdentifier[] = "\x0b"
                                    "C6-DOORBELL";

// Coordinator (the Zigbee2MQTT gateway) is always 0x0000, its first endpoint is 1.
constexpr uint16_t kCoordinatorShortAddress = 0x0000;
constexpr uint8_t kCoordinatorEndpoint = 1;

// Button.
constexpr int kButtonGpio = CONFIG_DOORBELL_BUTTON_GPIO;
constexpr uint32_t kDebounceMs = 25;
// How long to wait for the contact to open before going back to sleep. A held
// button must not keep the device awake forever.
constexpr uint32_t kMaxPressDurationMs = 10000;

// How long to wait for the network after boot before giving up (and keeping the
// press queued for the next wake-up).
constexpr uint32_t kNetworkWaitMs = 15000;
// Back-off when the network could not be reached.
constexpr uint32_t kRetrySleepS = 10;

// Time for the coordinator to confirm the reports before the radio is cut.
constexpr uint32_t kReportTimeoutMs = 2000;
constexpr uint32_t kDwellAfterReportMs = 250;

constexpr uint32_t kBootAwakeMs = static_cast<uint32_t>(CONFIG_DOORBELL_BOOT_AWAKE_S) * 1000U;
constexpr uint32_t kJoinAwakeMs = static_cast<uint32_t>(CONFIG_DOORBELL_JOIN_AWAKE_S) * 1000U;
constexpr uint32_t kKeepaliveWakeS = static_cast<uint32_t>(CONFIG_DOORBELL_KEEPALIVE_WAKE_S);

// Upper bound for queued (undelivered) presses held in RTC memory.
constexpr uint32_t kMaxPendingPresses = 5;

}  // namespace doorbell
