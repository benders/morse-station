// platform_nrf52.cpp — nRF52840 implementation of the platform:: seam.
//
// Mirror of platform_esp32.cpp for the RAK4631 (Phase 2 cross-MCU port). Wraps
// the four chip calls the application needs, using the Nordic SoftDevice /
// nRF5 SDK primitives bundled with the Adafruit nRF52 Arduino core:
//
//   NVIC_SystemReset()        → platform::restart()
//   sd_power_system_off()     → platform::system_off()
//   NRF_POWER->RESETREAS      → platform::reset_reason()  (bitfield, not enum)
//   NRF_FICR->DEVICEID[0..1]  → platform::unique_id_byte()
//
// Guarded to compile only for the RAK4631 target. See platform_esp32.cpp for
// the ESP32-S3 sibling (Heltec V4/V3, Cardputer ADV).
//
// IMPORTANT — reset_reason() encoding differs from ESP32:
// RESETREAS is a *bitfield* (multiple bits can be set on one boot — e.g. a
// brownout during a pin reset sets both bits) whereas esp_reset_reason_t is a
// small linear enum. We cannot reuse the ESP integer encoding here (the
// platform.h doc comment's "values must match esp_reset_reason_t" constraint
// applies only within a single MCU family — see the nRF52 table below), so
// reset_reason_label() is platform-provided and main.cpp no longer keeps its
// own copy (it now calls platform::reset_reason_label() — see platform.h).
// We map the bitfield down to a small linear enum of our own, choosing the
// most specific single cause when multiple bits are set (priority order below)
// so the boot banner and bootlog ring stay simple one-byte values.

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)

#include "platform.h"
#include <Arduino.h>
#include <nrf_power.h>   // NRF_POWER, RESETREAS bit masks
#include <nrf_sdm.h>     // sd_power_system_off()

namespace platform {

// Reboot immediately. NVIC_SystemReset() is the standard Cortex-M system
// reset; it does not return.
void restart() {
    NVIC_SystemReset();
}

// Enter the lowest-power "System OFF" state with no wake source configured —
// only a RESET pin pulse (or, on some boards, a configured GPIO wake) brings
// the unit back, which re-runs the whole sketch from the top (matching the
// ESP32 deep-sleep "RST to wake" behaviour the hibernate() banner promises).
//
// sd_power_system_off() is the SoftDevice-aware call (required when the S140
// SoftDevice is enabled — calling NRF_POWER->SYSTEMOFF directly while the
// SoftDevice is active is undefined behaviour). Fall back to the bare register
// write if the SoftDevice call ever returns (it shouldn't — System OFF halts
// the CPU — but the compiler doesn't know that).
void system_off() {
    sd_power_system_off();
    NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter << POWER_SYSTEMOFF_SYSTEMOFF_Pos;
    while (true) { __WFE(); }   // never reached on real hardware
}

// nRF52 reset-cause enum (our own small linear encoding — see header comment).
// Numeric values are local to this platform; reset_reason_label() below is the
// only thing that interprets them, and the bootlog ring stores whichever of
// these platform::reset_reason() returns, so the two stay in lockstep.
enum {
    RR_POWERON = 0,   // RESETREAS == 0 (no bits set: first power-up / brownout-only)
    RR_PIN     = 1,   // RESETPIN: external reset pin pulled low
    RR_DOG     = 2,   // DOG: watchdog timeout
    RR_SREQ    = 3,   // SREQ: NVIC_SystemReset() / soft reset
    RR_LOCKUP  = 4,   // LOCKUP: CPU lockup detected
    RR_OFF     = 5,   // OFF: wake from System OFF via GPIO/LPCOMP/NFC/VBUS
    RR_DIF     = 6,   // DIF: wake from System OFF via debug interface
    RR_NFC     = 7,   // NFC: wake from System OFF via NFC field
    RR_VBUS    = 8,   // VBUS: wake from System OFF via USB VBUS
    RR_UNKNOWN = 9,
};

// Read+clear RESETREAS (write-1-to-clear, per the nRF52840 reference manual)
// and fold the bitfield down to the single most-specific cause. Priority
// mirrors what's most useful for "why did we reboot": an explicit pin/SW reset
// or a watchdog/lockup is more diagnostic than "also browned out", so those
// take precedence over the System-OFF wake bits, which take precedence over
// the all-zero "clean power-on" case.
int reset_reason() {
    uint32_t r = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = r;   // write-1-to-clear so the next boot starts fresh

    if (r & POWER_RESETREAS_DOG_Msk)     return RR_DOG;
    if (r & POWER_RESETREAS_LOCKUP_Msk)  return RR_LOCKUP;
    if (r & POWER_RESETREAS_SREQ_Msk)    return RR_SREQ;
    if (r & POWER_RESETREAS_RESETPIN_Msk)return RR_PIN;
    if (r & POWER_RESETREAS_VBUS_Msk)    return RR_VBUS;
    if (r & POWER_RESETREAS_NFC_Msk)     return RR_NFC;
    if (r & POWER_RESETREAS_DIF_Msk)     return RR_DIF;
    if (r & POWER_RESETREAS_OFF_Msk)     return RR_OFF;
    if (r == 0)                          return RR_POWERON;
    return RR_UNKNOWN;
}

// Labels for the enum above — printed in the boot banner and `bootlog` dump.
const char* reset_reason_label(int x) {
    static const char* rr[] = {
        "POWERON", "PIN", "WATCHDOG", "SOFT", "LOCKUP",
        "OFF_WAKE", "DEBUG_WAKE", "NFC_WAKE", "VBUS_WAKE", "UNKNOWN",
    };
    return (x >= 0 && x < (int)(sizeof(rr) / sizeof(rr[0]))) ? rr[x] : "?";
}

// Derive a stable 1..254 station-ID byte from the factory FICR DEVICEID
// (a 64-bit factory-programmed unique ID, stable across reflashes — the nRF52
// equivalent of the ESP32 eFuse MAC). XOR-fold both 32-bit halves down to one
// byte the same way unique_id_byte() does on ESP32, then map into [1..254]
// (0 = "uninitialised"-looking, 255 = broadcast — both excluded).
uint8_t unique_id_byte() {
    uint32_t lo = NRF_FICR->DEVICEID[0];
    uint32_t hi = NRF_FICR->DEVICEID[1];
    uint32_t folded = lo ^ hi;
    uint8_t x = 0;
    for (int i = 0; i < 4; i++) x ^= (uint8_t)(folded >> (8 * i));
    return (uint8_t)(x % 254) + 1;
}

} // namespace platform

#endif // DEVICE_RAK4631 || DEVICE_WIO_TRACKER_L1
