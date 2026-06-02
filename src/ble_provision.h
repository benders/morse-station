#pragma once

#include <Print.h>

// BLE-UART (Nordic UART Service) provisioning transport. Stands up a NimBLE
// peripheral that advertises and bridges a generic BLE-UART terminal app
// (nRF Connect, LightBlue, Bluefruit Connect) to the existing line-based
// provisioning parser. Each received line is dispatched through the supplied
// handler with a Print& sink that notifies responses back over the TX
// characteristic. See docs/ble-provisioning.md.
//
// BLE and the SX1262 share the ESP32 radio core, so this is a boot-window-only
// activity: begin() before the LoRa radio comes up, stop() right before
// radio::init().
namespace ble_provision {

// Command dispatch callback, typed to match handle_setup_command(). Returns
// true when the session should end (unused by the BLE path — the session ends
// at radio init time regardless).
typedef bool (*Handler)(const char* line, Print& out);

// Start advertising as `adv_name`, routing received lines to `handler`.
void begin(const char* adv_name, Handler handler);

// Tear down NimBLE (NimBLEDevice::deinit(true)) so the radio core is free for
// the SX1262.
void stop();

}  // namespace ble_provision
