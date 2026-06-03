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

static float   s_freq    = 600.0f;
static uint8_t s_vol     = 255;
static bool    s_muted   = false;
static bool    s_want_on = false;   // last on/off request, so unmute can resume

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    cardputer_m5_begin();           // idempotent; M5.begin owns the codec
    s_freq = (float)freq_hz;
    M5.Speaker.setVolume(s_vol);
    // Power up the ES8311 + NS4150B amp here, NOT back in cardputer_m5_begin().
    // Its inrush was browning out the battery rail when stacked on the LCD/BLE
    // init at the very start of setup(); sidetone_init runs after the 3 s splash,
    // so the spike lands on a settled rail. Done once (begin() is idempotent via
    // _task_running), so the first keyed element carries no codec-begin latency.
    M5.Speaker.begin();
}

void sidetone_set_volume(uint8_t vol) {
    s_vol = vol;                    // 0 (faint) .. 255 (full), applied live
    M5.Speaker.setVolume(vol);
}

void sidetone_set_mute(bool m) {
    s_muted = m;
    if (m) M5.Speaker.stop();                       // silence a sounding tone now
    else if (s_want_on) M5.Speaker.tone(s_freq, UINT32_MAX);  // resume held key
}

void sidetone_on() {
    // duration UINT32_MAX => play until stopped. stop_current_sound=true so a
    // re-trigger mid-tone is seamless.
    s_want_on = true;
    if (!s_muted) M5.Speaker.tone(s_freq, UINT32_MAX);
}

void sidetone_off() {
    s_want_on = false;
    M5.Speaker.stop();
}

#endif // DEVICE_CARDPUTER_ADV
