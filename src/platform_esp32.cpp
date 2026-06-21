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
#include <esp_timer.h>    // esp_timer_* — the fixed-cadence keyer tick
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

// Drop the ESP32-S3 core clock to 80 MHz (from the 240 MHz arduino-esp32
// default). Morse keying is millisecond-scale and the SX1262 does the RF work,
// so 80 MHz is ample; the lower clock cuts active-mode current materially. The
// APB/peripheral clocks (UART, I2C, SPI) track the change transparently in
// arduino-esp32, so the serial console, OLED and radio SPI keep working.
//
// IPC-task overflow hazard (timing-sensitive): IDF dispatches some cross-core
// work (interrupt allocation during driver install, BLE controller bring-up,
// flash cache-disable for NVS writes) onto the per-core IPC task (ipc1), whose
// stack is a fixed ~1 KB (CONFIG_ESP_IPC_TASK_STACK_SIZE, baked into the
// precompiled framework). That budget is thin, and running the *boot init*
// window at 80 MHz widens the timing windows enough that two such dispatches can
// nest and overflow it — caught at the next context switch as an asynchronous
// "Stack canary watchpoint triggered (ipc1)" panic ~2 s into boot, with a
// corrupted vTaskSwitchContext/_frxt_dispatch backtrace (a ~2 s boot-loop).
// HW-confirmed deterministic on stn42 (V4.2). The mitigation is to call this at
// the END of setup() (see main.cpp), so all boot-time cross-core dispatch runs
// at 240 MHz; only the steady-state loop runs at 80 MHz. Use the `ipc` console
// command to check the live ipc0/ipc1 high-water margin.
void set_cpu_low_power() {
#if defined(DEVICE_CARDPUTER_ADV)
    // The Cardputer's M5Unified stack registers an APB-frequency-change callback
    // for its I2S/peripheral clocks. Dropping the core to 80 MHz re-enters that
    // registration (addApbChangeCallback "duplicate func") and trips the same
    // IPC-task stack-canary panic unconditionally — the board boot-loops ~10 s
    // in. Its idle floor is dominated by the LCD/codec, not the core clock, so
    // 80 MHz buys little here; keep the M5 board at its 240 MHz default.
#else
    setCpuFrequencyMhz(80);   // Heltec V3/V4: 80 MHz, applied late in setup()
#endif
}

uint32_t cpu_freq_mhz() { return getCpuFrequencyMhz(); }
int      cpu_cores()    { return ESP.getChipCores(); }

// Fixed-cadence keyer tick via esp_timer (TODO-fox-timing.md mitigation #1).
//
// esp_timer runs the callback from its own dedicated high-priority task (the
// "esp_timer" task), independent of the Arduino loopTask, so a slow LCD/audio/
// BLE loop() frame can no longer stretch a measured Morse element/gap. esp_timer
// is clock-source-agnostic (driven by the high-res timer, not the CPU APB
// scaling), so it does NOT retrigger the Cardputer M5/APB boot-loop hazard that
// setCpuFrequencyMhz() does (see set_cpu_low_power()).
//
// Power gating: the handle is created lazily once; keyer_start()/keyer_stop()
// map to esp_timer_start_periodic()/esp_timer_stop(), so the timer fires only
// while a message is keying and is genuinely stopped (no wake-ups) through the
// inter-message pause — keeping the sleep-through-pause door open.
static esp_timer_handle_t s_keyer_timer = nullptr;
static void (*s_keyer_cb)() = nullptr;
static bool s_keyer_running = false;

// esp_timer trampoline: fixed C-callable signature -> the registered void(void).
static void keyer_trampoline(void* /*arg*/) {
    if (s_keyer_cb) s_keyer_cb();
}

bool keyer_start(void (*cb)(), uint32_t period_us) {
    if (!cb) return false;
    s_keyer_cb = cb;
    if (s_keyer_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback        = keyer_trampoline;
        args.arg             = nullptr;
        args.dispatch_method = ESP_TIMER_TASK;   // run in the esp_timer task, not ISR
        args.name            = "keyer";
        if (esp_timer_create(&args, &s_keyer_timer) != ESP_OK) {
            s_keyer_timer = nullptr;
            return false;
        }
    }
    if (s_keyer_running) {
        // Already armed (re-arm without a stop): restart cleanly.
        esp_timer_stop(s_keyer_timer);
        s_keyer_running = false;
    }
    if (esp_timer_start_periodic(s_keyer_timer, period_us) != ESP_OK) return false;
    s_keyer_running = true;
    return true;
}

void keyer_stop() {
    if (s_keyer_timer != nullptr && s_keyer_running) {
        esp_timer_stop(s_keyer_timer);
        s_keyer_running = false;
    }
}

uint32_t keyer_now_us() { return (uint32_t)esp_timer_get_time(); }

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
