#pragma once
#ifdef DEVICE_CARDPUTER_ADV
#include <stdint.h>

// Cardputer ADV keyboard (TCA8418 I2C keypad controller, addr 0x34 on the
// internal bus GPIO8/9). Polled, non-blocking. Modifiers (shift/capslock) are
// tracked internally; ctrl/opt/alt are ignored. See keyboard_cardputer.cpp.

namespace keyboard {

// Configure the TCA8418 (matrix 7x8, event mode). Idempotent-safe to call after
// cardputer_m5_begin(); uses M5.In_I2C so it shares M5's bus, no second master.
void begin();

// If a key was pressed since the last call, set `c` and return true. Returns
// printable ASCII, or '\b' (del/backspace), '\r' (enter), '\t' (tab). Releases
// and modifier keys are consumed internally and never returned. Non-blocking.
bool read_char(char& c);

} // namespace keyboard
#endif // DEVICE_CARDPUTER_ADV
