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
#include <FreeRTOS.h>    // FreeRTOS kernel (Adafruit nRF52 core)
#include <task.h>        // xTaskCreate / vTaskDelayUntil / suspend / resume

namespace platform {

// Reboot immediately. NVIC_SystemReset() is the standard Cortex-M system
// reset; it does not return.
//
// NB: on the RAK4631 an intentional reboot is sometimes followed by a spurious
// second reset (the node boots twice — observed independent of this code, e.g.
// during the GPREGRET2 experiments). It self-settles (no boot loop), but the
// reboot-intent flag in main.cpp therefore logs the second boot as an unexpected
// WATCHDOG event. A real field watchdog (a wedged loop) is unaffected — it is a
// single reset and logs a single WATCHDOG. Root-causing the double-boot is a
// separate task (suspect: SoftDevice reset handling); sd_nvic_SystemReset() was
// tried and did NOT eliminate it.
void restart() {
    NVIC_SystemReset();
}

// The nRF52840 runs a fixed 64 MHz core clock with no software scaling, so the
// low-power-clock seam is a no-op here; cpu_freq_mhz() reports the constant and
// cpu_cores() the single Cortex-M4 core (the BLE stack runs on the same core via
// the SoftDevice, so there is no second core to idle).
void     set_cpu_low_power() { /* nRF52840: fixed 64 MHz, nothing to scale */ }
uint32_t cpu_freq_mhz()      { return 64; }
int      cpu_cores()         { return 1; }

// Fixed-cadence keyer tick via a dedicated FreeRTOS task (TODO-fox-timing.md
// mitigation #1). The task is created ONCE (on first keyer_start) at a priority
// above the Adafruit loopTask but below the SoftDevice, and `vTaskDelayUntil`s
// at the requested period — so a slow loop() can't stretch a measured Morse
// element/gap. `cb` must stay tiny (sample player + timestamp edges only) so it
// can never starve the radio/BLE stack.
//
// Power gating: keyer_start()/keyer_stop() resume/suspend the task. A suspended
// task adds no scheduler ticks, so FreeRTOS tickless idle / sd_app_evt_wait can
// reach low-power wait through the inter-message pause (sleep-through-pause).
static TaskHandle_t s_keyer_task = nullptr;
static void (*s_keyer_cb)()      = nullptr;
static uint32_t s_keyer_period_ticks = 1;
static volatile bool s_keyer_running = false;

static void keyer_task_body(void* /*arg*/) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, s_keyer_period_ticks);
        if (s_keyer_running && s_keyer_cb) s_keyer_cb();
    }
}

bool keyer_start(void (*cb)(), uint32_t period_us) {
    if (!cb) return false;
    s_keyer_cb = cb;
    uint32_t ticks = pdMS_TO_TICKS(period_us / 1000);
    if (ticks == 0) ticks = 1;       // 2 ms >= 1 tick at the 1024 Hz config tick
    s_keyer_period_ticks = ticks;
    if (s_keyer_task == nullptr) {
        // Priority above loopTask (TASK_PRIO_LOW), below the SoftDevice. Small
        // stack: the body only samples the player + pushes a ring entry.
        BaseType_t ok = xTaskCreate(keyer_task_body, "keyer", 256, nullptr,
                                    TASK_PRIO_HIGH, &s_keyer_task);
        if (ok != pdPASS) {
            s_keyer_task = nullptr;
            return false;
        }
        // Created in the running state; gate it explicitly via s_keyer_running.
    }
    s_keyer_running = true;
    vTaskResume(s_keyer_task);
    return true;
}

void keyer_stop() {
    if (s_keyer_task != nullptr) {
        s_keyer_running = false;
        vTaskSuspend(s_keyer_task);
    }
}

uint32_t keyer_now_us() { return micros(); }

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

// Reset codes for the application's flash reboot-intent synthesis (see
// platform.h / main.cpp). These return our RR_* values so reset_reason_label()
// renders them as the expected strings on this platform.
int reset_code_soft()     { return RR_SREQ; }   // "SOFT"
int reset_code_off()      { return RR_OFF;  }   // "OFF_WAKE"
int reset_code_watchdog() { return RR_DOG;  }   // "WATCHDOG"

// Labels for the enum above — printed in the boot banner and `bootlog` dump.
const char* reset_reason_label(int x) {
    static const char* rr[] = {
        "POWERON", "PIN", "WATCHDOG", "SOFT", "LOCKUP",
        "OFF_WAKE", "DEBUG_WAKE", "NFC_WAKE", "VBUS_WAKE", "UNKNOWN",
    };
    return (x >= 0 && x < (int)(sizeof(rr) / sizeof(rr[0]))) ? rr[x] : "?";
}

// Hardware watchdog via the nRF52840 WDT peripheral. Unlike the ESP32 task WDT,
// this is a free-running hardware counter clocked off the always-on 32.768 kHz
// LFCLK — it keeps counting even if the CPU, the SoftDevice, or an ISR wedges,
// and a timeout triggers a full chip reset. NB: on the Adafruit bootloader the
// reset cause does NOT survive to reset_reason() (the bootloader clears RESETREAS,
// and GPREGRET2/.noinit RAM are likewise wiped on a watchdog reset), so the
// "why did we reboot?" label is recovered at the application layer from a flash
// reboot-intent flag instead (see config::reboot_intent / main.cpp). The WDT is
// NOT one of the SoftDevice-restricted peripherals, so direct register access is
// safe with S140 enabled. Two silicon constraints shape this code:
//   - CRV/CONFIG/RREN can only be written BEFORE TASKS_START; once the watchdog
//     is running it cannot be reconfigured or stopped until the next reset. So
//     watchdog_begin() is one-shot (guarded by s_wdt_armed).
//   - HALT=Pause stops the counter while a debugger has the core halted (so a
//     breakpoint doesn't spuriously reset); SLEEP=Run keeps it counting in
//     System ON sleep, so a node that wedges while idle still reboots.
static bool s_wdt_armed = false;

void watchdog_begin(uint32_t timeout_ms) {
    if (s_wdt_armed) return;        // cannot reconfigure once started (silicon)
    NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) |
                      (WDT_CONFIG_SLEEP_Run  << WDT_CONFIG_SLEEP_Pos);
    // CRV counts down at 32.768 kHz; reset fires CRV+1 ticks after the last feed.
    uint64_t crv = ((uint64_t)timeout_ms * 32768ULL) / 1000ULL;
    if (crv < 1) crv = 1;
    NRF_WDT->CRV  = (uint32_t)(crv - 1);
    NRF_WDT->RREN = WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos;  // reload register 0
    NRF_WDT->TASKS_START = 1;
    s_wdt_armed = true;
}

void watchdog_feed() {
    if (s_wdt_armed) NRF_WDT->RR[0] = WDT_RR_RR_Reload;
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

// Full 64-bit factory FICR DEVICEID as a hex string (the nRF52 unique chip id).
const char* chip_id_str() {
    static char buf[20];
    uint32_t hi = NRF_FICR->DEVICEID[1];
    uint32_t lo = NRF_FICR->DEVICEID[0];
    snprintf(buf, sizeof(buf), "%08lX%08lX",
             (unsigned long)hi, (unsigned long)lo);
    return buf;
}

// SoC family. No silicon-revision read wired up here (FICR has it, but it isn't
// needed for board identity); the nRF52840 is the only nRF part in this project.
const char* soc_str() { return "nRF52840"; }

} // namespace platform

#endif // DEVICE_RAK4631 || DEVICE_WIO_TRACKER_L1
