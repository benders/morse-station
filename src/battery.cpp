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

namespace battery {

void begin() { cardputer_m5_begin(); }   // M5.begin() owns the PMIC

int percent() {
    int p = M5.Power.getBatteryLevel();   // 0..100, or -1 when unsupported
    if (p < 0) return -1;
    if (p > 100) p = 100;
    static uint32_t last_log = 0;         // rate-limit the log; percent() runs per frame
    uint32_t now = millis();
    if (now - last_log >= 2000) { last_log = now; Serial.printf("# batt %d%%\n", p); }
    return p;
}

bool charging() {
    return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

} // namespace battery

#else
// ---- Heltec V4: divider on PIN_VBAT_ADC, gated by PIN_VBAT_CTRL ------------
#include "pins.h"
#include <Arduino.h>

namespace {

constexpr float DIVIDER = 4.9f;        // (390k + 100k) / 100k
float  smoothed_v = 0.0f;              // EWMA of terminal voltage
uint32_t last_ms  = 0;

float read_volts() {
    // ADC-enable gate is active HIGH on Heltec V4 (both 4.2 and 4.3) — the
    // opposite of the V3 "GPIO37 low" convention. Verified on hardware: driving
    // GPIO37 HIGH puts the divided cell voltage on GPIO1 (~0.83 V at a 4.05 V
    // cell, x4.9 -> 4.05 V); driving it LOW reads 0 on both revisions. Pulse it
    // HIGH only for the sample, then park LOW so the divider doesn't draw.
    digitalWrite(PIN_VBAT_CTRL, HIGH); // connect the divider
    delay(10);
    // Average a few samples; analogReadMilliVolts applies the eFuse ADC cal.
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_VBAT_ADC);
    digitalWrite(PIN_VBAT_CTRL, LOW);  // disconnect to stop idle drain
    return (acc / 8.0f) / 1000.0f * DIVIDER;
}

} // namespace

namespace battery {

void begin() {
    pinMode(PIN_VBAT_CTRL, OUTPUT);
    digitalWrite(PIN_VBAT_CTRL, LOW);  // divider disconnected when idle (active-HIGH gate)
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

bool charging() { return false; }      // no charge-sense line wired

} // namespace battery

#endif
