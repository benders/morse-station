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
#include <esp_task_wdt.h> // esp_task_wdt_init/add/reset — the Task Watchdog Timer
#include <Arduino.h>      // ESP.getEfuseMac()

namespace platform {

// Hardware watchdog via the ESP-IDF Task Watchdog Timer (TWDT). The arduino-esp32
// core leaves the Arduino loopTask UNsubscribed by default, so a wedged loop()
// would never reboot. We re-init the TWDT with our own timeout, subscribe the
// calling task (loopTask, since watchdog_begin() runs at the end of setup()),
// and feed it from loop(). panic=true makes a timeout reboot the chip; the next
// boot's esp_reset_reason() reports TASK_WDT(6), captured in the bootlog ring.
static bool s_wdt_armed = false;

void watchdog_begin(uint32_t timeout_ms) {
    uint32_t secs = (timeout_ms + 999) / 1000;   // TWDT timeout is in whole seconds
    if (secs == 0) secs = 1;
    esp_task_wdt_init(secs, true);   // panic=true → reset on timeout
    esp_task_wdt_add(NULL);          // watch the calling task (the Arduino loopTask)
    esp_task_wdt_reset();
    s_wdt_armed = true;
}

void watchdog_feed() {
    if (s_wdt_armed) esp_task_wdt_reset();
}

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

// Reset codes matching esp_reset_reason_t. ESP32's reset_reason() is already
// accurate (the chip latches the cause and the ROM doesn't clear it), so the
// reboot-intent synthesis in main.cpp is nRF52-only and never consults these on
// ESP32 — they exist for seam symmetry. ESP_RST_SW=3, ESP_RST_DEEPSLEEP=8,
// ESP_RST_TASK_WDT=6.
int reset_code_soft()     { return 3; }   // ESP_RST_SW   -> "SW"
int reset_code_off()      { return 8; }   // ESP_RST_DEEPSLEEP -> "DEEPSLEEP"
int reset_code_watchdog() { return 6; }   // ESP_RST_TASK_WDT  -> "TASK_WDT"

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

// Full eFuse MAC as the conventional colon-separated string. ESP.getEfuseMac()
// packs the factory MAC with octet 0 in the LSB, so emitting bytes LSB-first
// reproduces esptool's read_mac order (e.g. "8C:FD:49:B6:75:5C").
const char* chip_id_str() {
    static char buf[18];
    uint64_t m = ESP.getEfuseMac();
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)(m), (uint8_t)(m >> 8), (uint8_t)(m >> 16),
             (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));
    return buf;
}

// SoC model + revision exactly as the Arduino core reports them. NB: on this
// core ESP.getChipRevision() returns only the wafer MAJOR (0 on our S3s) — it
// does NOT expose the minor that esptool prints as "v0.2" (esptool reads the
// WAFER_VERSION_MINOR eFuse directly). We print the raw int rather than
// fabricate a minor. SoC only: every ESP32-S3 board here (Heltec V4.2/V4.3, V3,
// Cardputer) reports the SAME string, so this cannot identify the board model.
const char* soc_str() {
    static char buf[28];
    snprintf(buf, sizeof(buf), "%s rev %d", ESP.getChipModel(),
             ESP.getChipRevision());
    return buf;
}

} // namespace platform

#endif // DEVICE_HELTEC_V4 || DEVICE_CARDPUTER_ADV
