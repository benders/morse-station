#include "sidetone.h"
#include <Arduino.h>
#include <math.h>

// Arduino-ESP32 2.0.x LEDC API (ledcSetup/ledcAttachPin/ledcWrite).
//
// Volume is set by the square wave's duty cycle: 50% is loudest, and shrinking
// the duty toward 0 lowers the RMS into the amp so the tone gets quieter (a
// PWM-style volume trick — there is no true DAC on the S3). We keep a current
// volume and apply it on each sidetone_on() so key-down picks up the latest
// level (the hunter retunes it from RSSI for an analog "signal = loudness" feel).
static constexpr int      LEDC_CHANNEL    = 0;
static constexpr uint8_t  LEDC_RESOLUTION = 10;          // bits
static constexpr uint32_t DUTY_MAX        = 1u << (LEDC_RESOLUTION - 1);  // 50% = loudest
static constexpr uint32_t DUTY_MIN        = 3;           // near-silent floor (weak signal)

static uint32_t s_duty = DUTY_MAX;   // current volume as a duty value
static bool     s_on   = false;

void sidetone_init(int gpio_pin, uint32_t freq_hz) {
    ledcSetup(LEDC_CHANNEL, freq_hz, LEDC_RESOLUTION);
    ledcAttachPin(gpio_pin, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);  // start silent
}

void sidetone_set_volume(uint8_t vol) {
    // Map 0..255 onto [DUTY_MIN, DUTY_MAX] with an exponential (dB-linear) curve
    // so equal input steps give equal *perceived* loudness steps. A linear duty
    // map crowds all the change into the flat top of the square wave's
    // sin(pi*duty) loudness response; the exponential spreads it across the
    // whole range, which is what makes the "signal = loudness" feel audible.
    float n = (float)vol / 255.0f;                       // 0..1
    float ratio = (float)DUTY_MAX / (float)DUTY_MIN;
    s_duty = (uint32_t)((float)DUTY_MIN * powf(ratio, n) + 0.5f);
    if (s_on) ledcWrite(LEDC_CHANNEL, s_duty);   // apply live if currently sounding
}

void sidetone_on() {
    s_on = true;
    ledcWrite(LEDC_CHANNEL, s_duty);
}

void sidetone_off() {
    s_on = false;
    ledcWrite(LEDC_CHANNEL, 0);
}
