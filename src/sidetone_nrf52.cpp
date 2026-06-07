// sidetone_nrf52.cpp — silent stub implementation of the sidetone:: API for
// the RAK4631.
//
// The RAK4631 / RAK19007 has no amp, codec, or piezo wired up in this port —
// see the "RAK4631 sidetone: none for now" decision in RAK-port-plan.md §0.
// Every symbol below is a no-op, but each still stores enough state that
// `show` (which prints the persisted volume/mute) and the BLE/serial `vol` /
// `mute` commands stay sane and round-trip correctly — only the actual sound
// is missing.
//
// TODO: a real backend would most likely be tone()-based on a GPIO driving a
// piezo buzzer (the simplest thing this MCU can do without an amp/codec); see
// sidetone.h for the API a future sidetone_nrf52_piezo.cpp would need to keep.
//
// Guarded to compile only for the RAK4631 target. The I2S/MAX98357A sibling
// (sidetone.cpp) is guarded to Heltec V4/V3; sidetone_cardputer.cpp self-guards
// to the Cardputer ADV.

#if defined(DEVICE_RAK4631)

#include "sidetone.h"
#include <stdint.h>

namespace {

bool    s_muted = false;
bool    s_on    = false;
uint8_t s_level = 16;     // mid-scale default, mirrors sidetone.cpp's GAIN_DEFAULT-ish
uint8_t s_volume = 255;   // RSSI-driven loudness curve input; full by default

} // namespace

void sidetone_init(int /*gpio_pin*/, uint32_t /*freq_hz*/) {
    // No hardware to configure. State above already has sane defaults; the
    // caller (main.cpp) immediately follows this with sidetone_set_level() /
    // sidetone_set_mute() restoring the persisted values, which we do store.
}

void sidetone_on()  { s_on = true; }
void sidetone_off() { s_on = false; }

void sidetone_set_volume(uint8_t vol) { s_volume = vol; }

void sidetone_set_level(uint8_t units) {
    if (units < 1)  units = 1;
    if (units > 32) units = 32;
    s_level = units;
}

void sidetone_set_mute(bool muted) { s_muted = muted; }

#endif // DEVICE_RAK4631
