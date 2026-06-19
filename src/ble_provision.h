#pragma once

#include <Print.h>

// BLE-UART (Nordic UART Service) provisioning transport. Stands up a NimBLE
// peripheral that advertises and bridges a generic BLE-UART terminal app
// (nRF Connect, LightBlue, Bluefruit Connect) to the existing line-based
// provisioning parser. Each received line is dispatched through the supplied
// handler with a Print& sink that notifies responses back over the TX
// characteristic. See docs/commands.md.
//
// Always-on: begin() during setup and leave NimBLE up for the whole session so
// an exercise operator can adjust parameters on a running node over the air
// (Step 2). The SX1262 is a separate SPI sub-GHz radio, so it coexists with the
// ESP32-S3 2.4 GHz BLE core; the only contention is heap and a little CPU. To
// keep config reads/writes single-threaded, the RX callback (NimBLE host task)
// only *queues* complete lines — process() drains and dispatches them from the
// caller's (main-loop) task, the same task that reads config.
namespace ble_provision {

// Command dispatch callback, typed to match handle_setup_command(). Returns
// true when the session should end (unused by the BLE path).
typedef bool (*Handler)(const char* line, Print& out);

// Start advertising as `adv_name`, routing received lines to `handler`. The
// handler is invoked from process(), not from the BLE callback.
void begin(const char* adv_name, Handler handler);

// Drain any RX lines received since the last call and dispatch each through the
// handler, notifying responses back over TX. Call once per main-loop iteration.
void process();

// Push an unsolicited line to the connected central over the TX characteristic,
// outside the command/response path. Used for asynchronous events the operator's
// phone should see without having issued a command — e.g. an Instructor relay
// ACK arriving from a distant fox. A trailing newline is appended. No-op when no
// central is connected. Safe to call from the main loop.
void notify(const char* line);

// True while a central is connected (advertising auto-restarts on disconnect).
bool connected();

// Tear down NimBLE (NimBLEDevice::deinit(false)). Not used in the always-on
// path; kept for callers that want to free the radio core (e.g. before deep
// sleep). deinit(false) avoids the clearAll double-free in NimBLE 1.4.x.
void stop();

}  // namespace ble_provision
