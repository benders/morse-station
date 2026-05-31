#ifdef DEVICE_CARDPUTER_ADV

#include "sidetone.h"
#include "platform_cardputer.h"
#include <M5Unified.h>

// Cardputer ADV sidetone via the ES8311 codec + NS4150B amp (M5.Speaker).
//
// Same API as the Heltec LEDC path: a fixed frequency set at init, gated on/off
// per key element, with an RSSI-driven volume for the hunter. M5.Speaker runs
// the I2S/codec on its own RTOS task, so on()/off() just start/stop a tone and
// carry no key-down latency.

static float   s_freq = 600.0f;
static uint8_t s_vol  = 255;

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    cardputer_m5_begin();           // idempotent; M5.begin owns the codec
    s_freq = (float)freq_hz;
    M5.Speaker.setVolume(s_vol);
}

void sidetone_set_volume(uint8_t vol) {
    s_vol = vol;                    // 0 (faint) .. 255 (full), applied live
    M5.Speaker.setVolume(vol);
}

void sidetone_on() {
    // duration UINT32_MAX => play until stopped. stop_current_sound=true so a
    // re-trigger mid-tone is seamless.
    M5.Speaker.tone(s_freq, UINT32_MAX);
}

void sidetone_off() {
    M5.Speaker.stop();
}

#endif // DEVICE_CARDPUTER_ADV
