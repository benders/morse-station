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
// Stable per physical chip across reflashes; the unique audit anchor that ties a
// (possibly human-set) board_model to the exact unit in `show`. Returns a
// pointer to a static buffer (overwritten on each call).
const char* chip_id_str();

// SoC family + silicon revision, e.g. "ESP32-S3 rev v0.2" (from
// ESP.getChipModel()/getChipRevision()) or "nRF52840". Identifies the
// microcontroller only — NOT the board model (all our ESP32-S3 boards report the
// same SoC), so it cannot distinguish Heltec V4.2 from V4.3. Static buffer.
const char* soc_str();

// Map a reset_reason() value to a short human-readable label for the boot
// banner and `bootlog` dump. Each platform supplies its own table (the ESP32
// and nRF52 reset-cause encodings are different small enums); main.cpp no
// longer keeps its own copy. Unknown values return "?".
const char* reset_reason_label(int reason);

} // namespace platform
