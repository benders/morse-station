#include "sidetone.h"
#include <Arduino.h>

// Arduino-ESP32 2.0.x LEDC API (ledcSetup/ledcAttachPin/ledcWrite).
static constexpr int      LEDC_CHANNEL    = 0;
static constexpr uint8_t  LEDC_RESOLUTION = 10;          // bits
static constexpr uint32_t DUTY_ON         = 1u << (LEDC_RESOLUTION - 1);  // ~50%

void sidetone_init(int gpio_pin, uint32_t freq_hz) {
    ledcSetup(LEDC_CHANNEL, freq_hz, LEDC_RESOLUTION);
    ledcAttachPin(gpio_pin, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);  // start silent
}

void sidetone_on() {
    ledcWrite(LEDC_CHANNEL, DUTY_ON);
}

void sidetone_off() {
    ledcWrite(LEDC_CHANNEL, 0);
}
