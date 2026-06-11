#include "battery.h"

namespace {

// Map a single-cell LiPo terminal voltage (volts) to a 0..100 percentage.
// A coarse piecewise curve over the usable 3.30..4.20 V range — good enough for
// a "how much is left" bar, without pretending to coulomb-count. Clamped at the
// ends so a freshly-charged or nearly-flat pack reads 100/0 rather than wrapping.
int volts_to_percent(float v) {
    struct Pt { float v; int pct; };
    static const Pt curve[] = {
        {3.30f,   0}, {3.50f,  10}, {3.60f,  20}, {3.70f,  35},
        {3.75f,  50}, {3.85f,  65}, {3.95f,  80}, {4.05f,  92}, {4.20f, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (v <= curve[0].v)     return 0;
    if (v >= curve[n - 1].v) return 100;
    for (int i = 1; i < n; i++) {
        if (v < curve[i].v) {
            float span = curve[i].v - curve[i - 1].v;
            float f = (v - curve[i - 1].v) / span;
            return curve[i - 1].pct + (int)(f * (curve[i].pct - curve[i - 1].pct) + 0.5f);
        }
    }
    return 100;
}

} // namespace

#ifdef DEVICE_CARDPUTER_ADV
// ---- Cardputer ADV: M5Unified PMIC ----------------------------------------
#include "platform_cardputer.h"
#include <M5Unified.h>

namespace {

float    smoothed_p = -1.0f;           // EWMA of level %, <0 until first read
uint32_t last_ms    = 0;

} // namespace

namespace battery {

void begin() {
    cardputer_m5_begin();              // M5.begin() owns the PMIC/ADC
    int p = M5.Power.getBatteryLevel();
    smoothed_p = (p < 0) ? -1.0f : (float)(p > 100 ? 100 : p);  // seed for a steady first frame
    last_ms = millis();
}

int percent() {
    // Mirror the Heltec path: re-read the gauge only every couple of seconds and
    // smooth it (EWMA), so the bar/colour don't jitter on ADC noise even though
    // callers poll us every display frame. M5.Power.getBatteryLevel() reads the
    // GPIO10 divider each call, so rate-limiting it also keeps that off the loop.
    uint32_t now = millis();
    if (now - last_ms >= 2000) {
        last_ms = now;
        int p = M5.Power.getBatteryLevel();   // 0..100, or -1 when unsupported
        if (p >= 0) {
            if (p > 100) p = 100;
            smoothed_p = smoothed_p >= 0.0f ? smoothed_p * 0.7f + p * 0.3f : (float)p;
            Serial.printf("# batt %d%% -> %d%%\n", p, (int)(smoothed_p + 0.5f));
        }
    }
    if (smoothed_p < 0.0f) return -1;
    return (int)(smoothed_p + 0.5f);
}

int millivolts() {
    int mv = M5.Power.getBatteryVoltage();   // GPIO10 ADC * 2.0, in mV
    return mv > 0 ? mv : -1;
}

bool charging() {
    return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

} // namespace battery

#elif defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)
// ---- Heltec V4 / V3: divider on PIN_VBAT_ADC, gated by PIN_VBAT_CTRL ------
//
// Both boards use a 390k/100k voltage divider on PIN_VBAT_ADC (GPIO1) gated by
// a MOSFET on PIN_VBAT_CTRL (GPIO37).  Both gates are active-HIGH:
//
//   Heltec V4 (BATT_GATE_ACTIVE_HIGH=1): drive GPIO37 HIGH to connect the
//     divider, LOW to disconnect.  Verified on hardware: HIGH puts ~0.83 V on
//     GPIO1 at a 4.05 V cell; LOW reads 0 on both V4.2 and V4.3.
//
//   Heltec V3 (BATT_GATE_ACTIVE_HIGH=1): same active-HIGH gate as the V4.
//     HARDWARE-VERIFIED 2026-06-11 on a real V3 with a cell: gate HIGH reads
//     821 mV on GPIO1 -> 4.03 V; gate LOW reads 0.  (An earlier schematic-based
//     "active-LOW" guess was wrong and left the meter stuck at -1.)
//
// The compile-time macro BATT_GATE_ACTIVE_HIGH (defined in pins.h per board)
// selects the correct connect/park levels.
#include "pins.h"
#include <Arduino.h>

namespace {

constexpr float DIVIDER = 4.9f;        // (390k + 100k) / 100k
float  smoothed_v = 0.0f;              // EWMA of terminal voltage
uint32_t last_ms  = 0;

// Gate levels derived from the per-board polarity macro.
// CONNECT  = the level that switches the MOSFET on (connects divider to ADC).
// PARK     = the level that switches it off (stops idle drain).
// When BATT_GATE_ACTIVE_HIGH=1: CONNECT=HIGH, PARK=LOW  (V4 behaviour, unchanged).
// When BATT_GATE_ACTIVE_HIGH=0: CONNECT=LOW,  PARK=HIGH (V3 behaviour).
#if BATT_GATE_ACTIVE_HIGH
static constexpr int GATE_CONNECT = HIGH;
static constexpr int GATE_PARK    = LOW;
#else
static constexpr int GATE_CONNECT = LOW;
static constexpr int GATE_PARK    = HIGH;
#endif

float read_volts() {
    // Pulse the gate to its connect level for the ADC sample, then park it.
    digitalWrite(PIN_VBAT_CTRL, GATE_CONNECT);
    delay(10);
    // Average a few samples; analogReadMilliVolts applies the eFuse ADC cal.
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_VBAT_ADC);
    digitalWrite(PIN_VBAT_CTRL, GATE_PARK);   // disconnect to stop idle drain
    return (acc / 8.0f) / 1000.0f * DIVIDER;
}

} // namespace

namespace battery {

void begin() {
    pinMode(PIN_VBAT_CTRL, OUTPUT);
    digitalWrite(PIN_VBAT_CTRL, GATE_PARK);   // divider disconnected when idle
    analogReadResolution(12);
    smoothed_v = read_volts();         // seed so the first frame is steady
    last_ms = millis();
}

int percent() {
    uint32_t now = millis();
    if (now - last_ms >= 2000) {       // a couple of seconds between reads
        last_ms = now;
        float v = read_volts();
        smoothed_v = smoothed_v > 0.0f ? smoothed_v * 0.7f + v * 0.3f : v;
        Serial.printf("# batt %.2fV -> %d%%\n", smoothed_v, volts_to_percent(smoothed_v));
    }
    if (smoothed_v <= 0.5f) return -1; // implausibly low: no cell / bad read
    return volts_to_percent(smoothed_v);
}

int millivolts() {
    float v = read_volts();             // fresh gated read, divider-corrected
    return v > 0.5f ? (int)(v * 1000.0f + 0.5f) : -1;
}

bool charging() { return false; }      // no charge-sense line wired

} // namespace battery

#elif defined(DEVICE_RAK4631)
// ---- RAK4631: analogRead through the RAK19007 base-board divider ----------
//
// PIN_VBAT_READ (AIN, GPIO5 / nRF52 analog input) sits on the RAK19007's
// battery-sense divider. TODO CALIBRATE: the exact divider ratio needs the
// RAK19007 schematic (we could not fetch it from this environment — see
// reference/rak4631/README.md). RAK's published reference firmware commonly
// uses a ratio in the ~2.0-2.06x range (a 100k/100k-class divider plus the
// nRF52's internal ADC reference/gain chain); 2.0f is used here as a
// documented placeholder pending a multimeter check against a real cell.
// Reuses the same volts_to_percent() curve as the ESP32 boards — the LiPo
// chemistry is identical, only the read path differs.
#include "pins.h"
#include <Arduino.h>

namespace {

constexpr float DIVIDER_RAK = 2.0f;     // TODO CONFIRM against RAK19007 schematic
constexpr float ADC_REF_V   = 3.6f;     // nRF52 ADC default reference (internal 0.6V * gain 1/6)
constexpr int   ADC_MAX     = 4095;     // 12-bit

float  smoothed_v = 0.0f;
uint32_t last_ms  = 0;

float read_volts() {
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogRead(PIN_VBAT_READ);
    float raw = acc / 8.0f;
    return (raw / ADC_MAX) * ADC_REF_V * DIVIDER_RAK;
}

} // namespace

namespace battery {

void begin() {
    analogReadResolution(12);
    analogReference(AR_INTERNAL);          // ~0.6V internal reference
    smoothed_v = read_volts();             // seed so the first frame is steady
    last_ms = millis();
}

int percent() {
    uint32_t now = millis();
    if (now - last_ms >= 2000) {
        last_ms = now;
        float v = read_volts();
        smoothed_v = smoothed_v > 0.0f ? smoothed_v * 0.7f + v * 0.3f : v;
        Serial.printf("# batt %.2fV -> %d%%\n", smoothed_v, volts_to_percent(smoothed_v));
    }
    if (smoothed_v <= 0.5f) return -1;
    return volts_to_percent(smoothed_v);
}

int millivolts() {
    float v = read_volts();
    return v > 0.5f ? (int)(v * 1000.0f + 0.5f) : -1;
}

bool charging() { return false; }      // no charge-sense line wired in this port

} // namespace battery

#elif defined(DEVICE_WIO_TRACKER_L1)
// ---- Wio Tracker L1 Pro: gated divider on PIN_VBAT_ADC, nRF52 analogRead --
//
// Like the Heltec, this board gates its VBAT divider with a MOSFET
// (PIN_VBAT_CTRL, net BAT_CTL) rather than reading continuously like the RAK.
// pins.h defines BATT_GATE_ACTIVE_HIGH=1 for this board (drive HIGH to connect
// the divider, LOW to park/disconnect) — reuse the same GATE_CONNECT/GATE_PARK
// idiom as the Heltec branch, but convert counts -> volts using the RAK's
// nRF52 ADC reference math (internal 0.6V * 1/6 gain == 3.6V full-scale @ 12-bit).
//
// DIVIDER_WIO ~= 2.0, taken from the upstream Seeed/Meshtastic variant's
// ADC_MULTIPLIER for this board. TODO CONFIRM/CALIBRATE on real hardware (W9).
#include "pins.h"
#include <Arduino.h>

namespace {

constexpr float DIVIDER_WIO = 2.0f;     // TODO CONFIRM against schematic / bench (W9)
constexpr float ADC_REF_V   = 3.6f;     // nRF52 ADC default reference (internal 0.6V * gain 1/6)
constexpr int   ADC_MAX     = 4095;     // 12-bit

float  smoothed_v = 0.0f;
uint32_t last_ms  = 0;

// Gate levels derived from the per-board polarity macro (BATT_GATE_ACTIVE_HIGH=1
// for the Wio: CONNECT=HIGH, PARK=LOW).
#if BATT_GATE_ACTIVE_HIGH
static constexpr int GATE_CONNECT = HIGH;
static constexpr int GATE_PARK    = LOW;
#else
static constexpr int GATE_CONNECT = LOW;
static constexpr int GATE_PARK    = HIGH;
#endif

float read_volts() {
    digitalWrite(PIN_VBAT_CTRL, GATE_CONNECT);
    delay(10);                          // let the divider settle
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogRead(PIN_VBAT_ADC);
    digitalWrite(PIN_VBAT_CTRL, GATE_PARK);   // disconnect to stop idle drain
    float raw = acc / 8.0f;
    return (raw / ADC_MAX) * ADC_REF_V * DIVIDER_WIO;
}

} // namespace

namespace battery {

void begin() {
    pinMode(PIN_VBAT_CTRL, OUTPUT);
    digitalWrite(PIN_VBAT_CTRL, GATE_PARK);   // divider disconnected when idle
    analogReadResolution(12);
    smoothed_v = read_volts();         // seed so the first frame is steady
    last_ms = millis();
}

int percent() {
    uint32_t now = millis();
    if (now - last_ms >= 2000) {
        last_ms = now;
        float v = read_volts();
        smoothed_v = smoothed_v > 0.0f ? smoothed_v * 0.7f + v * 0.3f : v;
        Serial.printf("# batt %.2fV -> %d%%\n", smoothed_v, volts_to_percent(smoothed_v));
    }
    if (smoothed_v <= 0.5f) return -1;
    return volts_to_percent(smoothed_v);
}

int millivolts() {
    float v = read_volts();
    return v > 0.5f ? (int)(v * 1000.0f + 0.5f) : -1;
}

bool charging() { return false; }      // no charge-sense line wired in this port

} // namespace battery

#endif
