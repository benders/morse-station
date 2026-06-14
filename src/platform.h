#pragma once
#include <stdint.h>

// platform.h — chip-call seam for restart / sleep / reset-reason / device-ID.
//
// This header decouples the application from ESP32-specific APIs
// (esp_restart, esp_deep_sleep_start, esp_reset_reason, ESP.getEfuseMac)
// so that a future nRF52 port can supply identical symbols without touching
// any application code. Each target provides exactly one implementation file:
//   src/platform_esp32.cpp  — ESP32-S3 (Heltec V4, Cardputer ADV, Heltec V3)
//   src/platform_nrf52.cpp  — nRF52840 (RAK4631, Phase 2)
//
// The integer returned by reset_reason() MUST stay numerically identical to the
// esp_reset_reason() values (0..10) because reset_reason_label() in main.cpp
// indexes into a fixed table built around those values. The nRF52 platform file
// will supply its own label table (Phase 2).

namespace platform {

// Reboot the MCU immediately (no return).
void restart();

// Enter the deepest sleep state with no wake source.
// On ESP32: esp_deep_sleep_start() (only RST wakes).
// On nRF52: sd_power_system_off() (only RESETPIN/button wakes).
// Does not return.
void system_off();

// Return the numeric reset reason for this boot. The ESP32 numbering is used
// as the canonical encoding; reset_reason_label() in main.cpp maps it to a
// string. Values must match esp_reset_reason_t:
//   0=UNKNOWN 1=POWERON 2=EXT 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT
//   7=WDT 8=DEEPSLEEP 9=BROWNOUT 10=SDIO
int reset_reason();

// Return a stable 1-byte device identifier derived from the factory device ID
// (eFuse MAC on ESP32, FICR DEVICEID on nRF52). Guaranteed in range 1..254
// (255 is the broadcast address). Used as the default station ID when no
// provisioned value exists in NVS.
uint8_t unique_id_byte();

// Full factory hardware id as a hex string — eFuse MAC on ESP32 (colon form,
// e.g. "8C:FD:49:B6:75:5C", matching esptool read_mac), FICR DEVICEID on nRF52.
// Stable per physical chip across reflashes; the unique audit anchor that ties
// the auto-detected board model to the exact unit in `show`. Returns a pointer
// to a static buffer (overwritten on each call).
const char* chip_id_str();

// SoC family + silicon revision, e.g. "ESP32-S3 rev v0.2" (from
// ESP.getChipModel()/getChipRevision()) or "nRF52840". Identifies the
// microcontroller only — NOT the board model (all our ESP32-S3 boards report the
// same SoC), so it cannot distinguish Heltec V4.2 from V4.3. Static buffer.
const char* soc_str();

// Hardware watchdog — field resiliency. watchdog_begin() arms a hardware
// watchdog that reboots the MCU if watchdog_feed() is not called within
// timeout_ms; call it once after boot setup completes (so the long splash/menu
// delays and the blocking run_menu() don't trip it), then feed it every pass of
// the main loop. A watchdog reboot is recorded in `bootlog` as a field lockup
// after the fact: directly via reset_reason() on ESP32 (TASK_WDT), and on nRF52
// via the flash reboot-intent flag in main.cpp (the Adafruit bootloader clears
// the hardware reset-cause register, so reset_reason() can't see it — see
// reset_code_*() below). On nRF52 the watchdog peripheral cannot be reconfigured
// once started (silicon limitation) so begin() is effectively one-shot; the
// ESP32 task-WDT tolerates a repeat init harmlessly. Each platform supplies its
// own implementation (esp_task_wdt on ESP32, NRF_WDT registers on nRF52).
void watchdog_begin(uint32_t timeout_ms);

// Pet the watchdog so it does not fire. No-op if watchdog_begin() was never
// called (e.g. on the hibernate path, which never reaches the run loop).
void watchdog_feed();

// Map a reset_reason() value to a short human-readable label for the boot
// banner and `bootlog` dump. Each platform supplies its own table (the ESP32
// and nRF52 reset-cause encodings are different small enums); main.cpp no
// longer keeps its own copy. Unknown values return "?".
const char* reset_reason_label(int reason);

// reset_reason() codes for *synthesizing* a cause when the hardware reset-cause
// register is unreliable — specifically nRF52 under the Adafruit bootloader,
// which clears RESETREAS so reset_reason() always reports POWERON. main.cpp keeps
// a small flash "reboot intent" flag (set before an intentional restart()/
// system_off(), armed to RUNNING each boot) and maps it onto these codes so the
// boot banner / `bootlog` still distinguish a soft reboot, a hibernate wake, and
// an unexpected (watchdog/crash) reset. Each platform returns a value matching
// its own reset_reason()/reset_reason_label() encoding (meaningful only within
// one MCU family). On ESP32 reset_reason() is already accurate, so these are
// provided only for symmetry.
int reset_code_soft();        // intentional restart() / soft reset  -> "SOFT"
int reset_code_off();         // wake from system_off() / hibernate  -> "OFF_WAKE"/"DEEPSLEEP"
int reset_code_watchdog();    // unexpected reset (watchdog / crash)  -> "WATCHDOG"/"TASK_WDT"

// Low-power CPU clock (battery saver). set_cpu_low_power() drops the core to the
// lowest clock that still keeps the radio + Morse-keying timing correct: 80 MHz
// on the ESP32-S3 (down from the 240 MHz arduino-esp32 default — saves tens of
// mA), a no-op on the nRF52840 (fixed 64 MHz). Call once early in setup().
// cpu_freq_mhz() reports the running clock and cpu_cores() the physical core
// count, surfaced by the `power` console command for unattended verification.
void     set_cpu_low_power();
uint32_t cpu_freq_mhz();
int      cpu_cores();

} // namespace platform
