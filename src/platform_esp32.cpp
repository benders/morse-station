// platform_esp32.cpp — ESP32-S3 implementation of the platform:: seam.
//
// Wraps the four ESP-IDF/arduino-esp32 chip calls that the application needs:
//   esp_restart()        → platform::restart()
//   esp_deep_sleep_start()→ platform::system_off()
//   esp_reset_reason()   → platform::reset_reason()
//   ESP.getEfuseMac()    → platform::unique_id_byte()
//
// Guarded to compile only when one of the ESP32-S3 device targets is active.
// A future nRF52 target supplies its own platform_nrf52.cpp with identical
// function signatures but different chip API calls (Phase 2).
//
// reset_reason() returns the raw (int)esp_reset_reason() value so the table in
// reset_reason_label() (main.cpp) continues to work without any changes.

#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) || defined(DEVICE_CARDPUTER_ADV)

#include "platform.h"
#include <esp_sleep.h>    // esp_deep_sleep_start()
#include <esp_system.h>   // esp_restart(), esp_reset_reason()
#include <Arduino.h>      // ESP.getEfuseMac()

namespace platform {

// Reboot immediately via the ESP-IDF software reset.
void restart() {
    esp_restart();
}

// Enter deep sleep with no wake source configured.  Only a hardware RST (or
// power cycle) brings the node back; on-return is impossible, but the compiler
// does not know that, so the function falls off the end harmlessly.
void system_off() {
    esp_deep_sleep_start();
}

// Return the ESP reset reason as a plain int.  The esp_reset_reason_t enum
// values are:
//   ESP_RST_UNKNOWN=0, ESP_RST_POWERON=1, ESP_RST_EXT=2, ESP_RST_SW=3,
//   ESP_RST_PANIC=4, ESP_RST_INT_WDT=5, ESP_RST_TASK_WDT=6, ESP_RST_WDT=7,
//   ESP_RST_DEEPSLEEP=8, ESP_RST_BROWNOUT=9, ESP_RST_SDIO=10.
// These numeric values are the canonical encoding used by reset_reason_label()
// in main.cpp; do not remap them.
int reset_reason() {
    return (int)esp_reset_reason();
}

// Labels indexed by the esp_reset_reason_t values above (0..10). This table
// used to live in main.cpp's reset_reason_label(); each platform now supplies
// its own (the nRF52 RESETREAS-bit mapping is a different small enum — see
// platform_nrf52.cpp).
const char* reset_reason_label(int x) {
    static const char* rr[] = {"UNKNOWN","POWERON","EXT","SW","PANIC",
                               "INT_WDT","TASK_WDT","WDT","DEEPSLEEP",
                               "BROWNOUT","SDIO"};
    return (x >= 0 && x < (int)(sizeof(rr) / sizeof(rr[0]))) ? rr[x] : "?";
}

// Derive a stable 1..254 station-ID byte from the factory eFuse MAC.
// The MAC is a 48-bit value; XOR all six bytes together, then map into
// [1..254] (0 and 255 are excluded: 0 looks like "uninitialised", 255 is
// the broadcast address). The fold is stable across resets and reflashes
// that preserve the eFuse — i.e. it changes only on a chip replacement.
// See espressif/arduino-esp32#932 for the getEfuseMac() provenance.
uint8_t unique_id_byte() {
    uint64_t mac = ESP.getEfuseMac();
    uint8_t x = 0;
    for (int i = 0; i < 6; i++) x ^= (uint8_t)(mac >> (8 * i));
    return (uint8_t)(x % 254) + 1;
}

} // namespace platform

#endif // DEVICE_HELTEC_V4 || DEVICE_CARDPUTER_ADV
