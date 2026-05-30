#pragma once
#include <stdint.h>

// Debounced telegraph-key input on a GPIO wired key->GND with INPUT_PULLUP.
// Poll update() often (every loop); it returns the current debounced state and
// edges can be read via the pressed()/released() one-shot flags.

class MorseKey {
public:
    void begin(int gpio_pin, uint16_t debounce_ms = 5);
    void update();                 // call every loop
    bool down() const { return state_; }   // debounced key-down

private:
    int      pin_         = -1;
    uint16_t debounce_ms_ = 5;
    bool     state_       = false; // debounced
    bool     raw_last_    = false;
    uint32_t last_change_ = 0;
};
