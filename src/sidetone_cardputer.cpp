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
static bool    s_alert   = false;   // instructor alert overrides mute
static bool    s_want_on = false;   // last on/off request, so unmute can resume

// ES8311 codec I2C address on the Cardputer ADV internal bus (GPIO8/9 = M5.In_I2C),
// and the register write speed. Mirrors M5Unified's es8311_i2c_addr0.
static constexpr uint8_t ES8311_ADDR    = 0x18;
static constexpr uint32_t ES8311_I2C_HZ = 400000;

static inline void es8311_w(uint8_t reg, uint8_t val) {
    M5.In_I2C.writeRegister8(ES8311_ADDR, reg, val, ES8311_I2C_HZ);
}

// Bring the ES8311 analog stage up the anti-pop way, BEFORE M5.Speaker.begin().
//
// The pop on boot is M5Unified's speaker-enable callback: it powers up the
// analog circuitry (0x0D), the DAC (0x12) and ENABLES the HP output driver
// (0x13) while the DAC volume is already at full 0 dB (0x32=0xBF) — all in one
// bulk write, and (per Speaker_Class::begin) before I2S/MCLK even starts. VMID
// charges from 0 straight into a connected output stage -> a hard thump that no
// digital volume can mute, because it's the analog bias inrush, not signal.
//
// Textbook ES8311 anti-pop: charge VMID with the output driver DISCONNECTED, let
// it settle to mid-rail quietly, THEN connect the HP driver and ramp the DAC
// volume up. We issue M5's own register values (so the subsequent
// M5.Speaker.begin() bulk write is all no-ops and triggers no further analog
// transition), just reordered in time with 0x13 held off across the settle.
static void es8311_antipop_power_up() {
    es8311_w(0x00, 0x80);   // RESET: CSM power on
    es8311_w(0x01, 0xB5);   // CLOCK_MANAGER: MCLK=BCLK
    es8311_w(0x02, 0x18);   // CLOCK_MANAGER: MULT_PRE=3
    es8311_w(0x32, 0x00);   // DAC volume = mute (ramp up later)
    es8311_w(0x13, 0x00);   // HP output driver OFF — disconnect output stage
    es8311_w(0x0D, 0x01);   // power up analog circuitry: VMID charges, output off
    es8311_w(0x12, 0x00);   // power up DAC
    delay(150);             // let VMID settle to mid-rail with the output muted
    es8311_w(0x13, 0x10);   // now connect the HP output driver (VMID already up)
    delay(20);
    es8311_w(0x32, 0x40);   // soft-ramp DAC volume to 0 dB in a few steps
    delay(8);
    es8311_w(0x32, 0x80);
    delay(8);
    es8311_w(0x32, 0xBF);   // 0xBF == +-0 dB (M5's target value)
}

static bool s_codec_up = false;     // ES8311/amp powered + I2S running yet?

// Bring the codec + amp up exactly once, the anti-pop way. Deferred out of boot
// (see sidetone_init): the boot pop the operator hears "as the mode menu draws"
// is this bring-up. Calling it lazily — on the first tone/alert instead of at
// setup() — moves any unavoidable turn-on transient off the restart path, which
// is what matters for an Instructor that restarts in the field and rarely tones.
static void sidetone_codec_bringup() {
    if (s_codec_up) return;
    s_codec_up = true;
    es8311_antipop_power_up();
    M5.Speaker.setVolume(0);
    M5.Speaker.begin();             // re-issues settled values (no-op) + starts I2S
    delay(10);
    M5.Speaker.setVolume(s_vol);
}

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    cardputer_m5_begin();           // idempotent; M5.begin owns the codec
    s_freq = (float)freq_hz;
    // NOTE: the ES8311 + NS4150B amp are NOT powered here any more. Doing it at
    // boot put the turn-on transient right where the operator notices it (as the
    // mode menu appears on restart). The diagnostic proved the codec register
    // sequence is a clean no-op by the time M5.Speaker.begin() runs, so the pop
    // is the analog VMID/amp turn-on itself, not the digital path — and the only
    // way to keep it off the restart is to not turn the amp on at boot. Bring-up
    // now happens lazily in sidetone_codec_bringup() on the first tone/alert.
    // (The earlier brownout fix that moved begin() out of cardputer_m5_begin()
    // still holds — this just defers it further.)
}

void sidetone_set_volume(uint8_t vol) {
    s_vol = vol;                    // 0 (faint) .. 255 (full), applied live
    M5.Speaker.setVolume(vol);
}

void sidetone_set_level(uint8_t units) {
    if (units < 1)  units = 1;
    if (units > 32) units = 32;     // mirror the Heltec 1..32 GAIN_Q15/1024 range
    sidetone_set_volume((uint8_t)((units * 255 + 16) / 32));   // map onto 0..255
}

void sidetone_set_mute(bool m) {
    s_muted = m;
    if (s_alert) return;                            // alert tone holds the speaker
    if (m) M5.Speaker.stop();                       // silence a sounding tone now
    else if (s_want_on) M5.Speaker.tone(s_freq, UINT32_MAX);  // resume held key
}

void sidetone_alert(bool on) {
    s_alert = on;
    if (on) sidetone_codec_bringup();               // first sound powers the amp
    if (on) M5.Speaker.tone(s_freq, UINT32_MAX);    // sound now, regardless of mute
    else if (s_want_on && !s_muted) M5.Speaker.tone(s_freq, UINT32_MAX);  // resume key
    else M5.Speaker.stop();                         // back to silence / mute
}

void sidetone_on() {
    // duration UINT32_MAX => play until stopped. stop_current_sound=true so a
    // re-trigger mid-tone is seamless.
    s_want_on = true;
    if (!s_muted) { sidetone_codec_bringup(); M5.Speaker.tone(s_freq, UINT32_MAX); }
}

void sidetone_off() {
    s_want_on = false;
    M5.Speaker.stop();
}

#endif // DEVICE_CARDPUTER_ADV
