#include "sidetone.h"
#include <Arduino.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Sidetone generator.
//
// Default path (option 1, "pseudo-DAC"): the LEDC peripheral free-runs a
// high-frequency PWM carrier; a hardware-timer ISR rewrites the carrier's duty
// at an audio sample rate to trace a sine wave (DDS). The existing RC low-pass
// (~1k + 100nF) into the PAM8403 averages the carrier back into an analog sine.
//
// Volume is the *amplitude* of that sine, so the timbre is identical at every
// level. This fixes the "tinny at low volume" of the old duty-cycle scheme,
// where shrinking the duty pushed energy into high harmonics instead of just
// lowering loudness. The hunter's RSSI->loudness feature wants exactly this:
// a clean, constant-timbre gradient.
//
// The old square-wave duty-cycle path is kept under -DSIDETONE_SQUARE.
// ---------------------------------------------------------------------------

static constexpr int      LEDC_CHANNEL    = 0;

#ifdef SIDETONE_SQUARE
// -------- Legacy square-wave path (duty-cycle volume) ----------------------
static constexpr uint8_t  LEDC_RESOLUTION = 10;          // bits
static constexpr uint32_t DUTY_MAX        = 1u << (LEDC_RESOLUTION - 1);  // 50% = loudest
static constexpr uint32_t DUTY_MIN        = 3;           // near-silent floor

static uint32_t s_duty = DUTY_MAX;
static bool     s_on   = false;

void sidetone_init(int gpio_pin, uint32_t freq_hz) {
    ledcSetup(LEDC_CHANNEL, freq_hz, LEDC_RESOLUTION);
    ledcAttachPin(gpio_pin, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);
}

void sidetone_set_volume(uint8_t vol) {
    float n = (float)vol / 255.0f;
    float ratio = (float)DUTY_MAX / (float)DUTY_MIN;
    s_duty = (uint32_t)((float)DUTY_MIN * powf(ratio, n) + 0.5f);
    if (s_on) ledcWrite(LEDC_CHANNEL, s_duty);
}

void sidetone_on()  { s_on = true;  ledcWrite(LEDC_CHANNEL, s_duty); }
void sidetone_off() { s_on = false; ledcWrite(LEDC_CHANNEL, 0); }

#else
// -------- PWM sine synthesis path (amplitude volume) -----------------------
#include "soc/ledc_struct.h"   // direct duty register write from the IRAM ISR

static constexpr uint8_t  LEDC_RES_BITS = 8;             // 256 PWM levels
static constexpr uint32_t CARRIER_HZ    = 312500;        // 80 MHz / 256, well above audio
static constexpr uint32_t SAMPLE_RATE   = 25000;         // ISR rate (40 us, exact at 1 MHz tick)
static constexpr int32_t  DUTY_MID      = 128;           // 50% duty = AC zero crossing

// 256-entry signed sine, -127..127, indexed by the top 8 bits of the phase.
static int8_t           s_sine[256];
static volatile uint32_t s_phase     = 0;
static volatile uint32_t s_phase_inc = 0;                // DDS step per sample (Q32)
// Amplitude gain in Q8 (256 = full swing). Driven by sidetone_set_volume().
static volatile int32_t  s_gain_q8   = 256;
static volatile bool     s_on        = false;
static hw_timer_t*       s_timer     = nullptr;

// Write the LEDC duty register directly so the ISR touches no flash-resident
// code (safe even if the flash cache is momentarily disabled, e.g. during an
// NVS write). ESP32-S3 low-speed LEDC: duty is stored left-shifted by 4 (4
// fractional bits); duty_start re-arms the (zero-length, set at init) "fade"
// and low_speed_update commits it on the next carrier period. The fade params
// (duty_num/cycle/scale) were zeroed by the init ledcWrite(), so this is a
// plain immediate duty change.
static inline void IRAM_ATTR ledc_write_duty(uint32_t duty) {
    auto& ch = LEDC.channel_group[0].channel[LEDC_CHANNEL];
    ch.duty.duty          = duty << 4;
    ch.conf1.duty_start   = 1;
    ch.conf0.low_speed_update = 1;
}

static void IRAM_ATTR on_sample() {
    if (!s_on) return;
    s_phase += s_phase_inc;
    uint8_t idx = (uint8_t)(s_phase >> 24);
    int32_t amp = ((int32_t)s_sine[idx] * s_gain_q8) >> 8;   // -127..127 scaled
    ledc_write_duty((uint32_t)(DUTY_MID + amp));
}

void sidetone_init(int gpio_pin, uint32_t freq_hz) {
    for (int i = 0; i < 256; i++)
        s_sine[i] = (int8_t)lrintf(127.0f * sinf(2.0f * (float)M_PI * i / 256.0f));

    s_phase_inc = (uint32_t)(((uint64_t)freq_hz << 32) / SAMPLE_RATE);

    ledcSetup(LEDC_CHANNEL, CARRIER_HZ, LEDC_RES_BITS);
    ledcAttachPin(gpio_pin, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, DUTY_MID);   // idle at mid-rail (AC silent)

    // 1 MHz tick (prescaler 80), fire every SAMPLE_RATE; ISR free-runs and is
    // gated by s_on so key down/up has no setup latency.
    s_timer = timerBegin(0, 80, true);
    timerAttachInterrupt(s_timer, &on_sample, true);
    timerAlarmWrite(s_timer, 1000000UL / SAMPLE_RATE, true);
    timerAlarmEnable(s_timer);
}

void sidetone_set_volume(uint8_t vol) {
    // Map 0..255 onto the amplitude gain with an exponential (dB-linear) curve
    // so equal input steps give equal *perceived* loudness steps.
    constexpr float GAIN_MAX = 256.0f;   // full swing
    constexpr float GAIN_MIN = 4.0f;     // faint floor (weak signal)
    float n = (float)vol / 255.0f;
    s_gain_q8 = (int32_t)(GAIN_MIN * powf(GAIN_MAX / GAIN_MIN, n) + 0.5f);
}

void sidetone_on() {
    s_phase = 0;        // start at a zero crossing -> no click
    s_on = true;
}

void sidetone_off() {
    s_on = false;
    ledc_write_duty(DUTY_MID);   // park at mid-rail (AC silent)
}

#endif // SIDETONE_SQUARE
