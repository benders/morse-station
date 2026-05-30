#include "morsekey.h"
#include <Arduino.h>

void MorseKey::begin(int gpio_pin, uint16_t debounce_ms) {
    pin_         = gpio_pin;
    debounce_ms_ = debounce_ms;
    pinMode(pin_, INPUT_PULLUP);
    bool raw  = (digitalRead(pin_) == LOW);   // LOW = pressed (key shorts to GND)
    state_    = raw;
    raw_last_ = raw;
    last_change_ = millis();
}

void MorseKey::update() {
    bool raw = (digitalRead(pin_) == LOW);
    uint32_t now = millis();
    if (raw != raw_last_) {
        raw_last_    = raw;
        last_change_ = now;          // reset settling timer on any bounce
    }
    // Accept the new level once it has been stable past the debounce window.
    if (raw != state_ && (now - last_change_) >= debounce_ms_) {
        state_ = raw;
    }
}
