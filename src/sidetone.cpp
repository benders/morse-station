#include "sidetone.h"
#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)
#include <Arduino.h>
#include "pins.h"

#if defined(SIDETONE_BUZZER)
// ---------------------------------------------------------------------------
// Sidetone generator (passive piezo buzzer on a single PWM pin).
//
// Selected by -DSIDETONE_BUZZER (the ESP32 sibling of the nRF52 Wio buzzer path
// in sidetone_nrf52.cpp). Instead of the MAX98357A/I2S amp, drive a passive
// piezo buzzer directly from one GPIO (PIN_SIDETONE = PIN_SIDETONE_PWM, GPIO4)
// using LEDC, the ESP32 hardware PWM. A passive piezo makes no sound from a DC
// level — it needs an AC drive at the tone frequency, which a 50%-duty square
// wave at freq_hz provides. on/off just gates the duty, so key-down/up has no
// retune latency (same contract as the I2S path).
//
// Loudness control on a two-level square is crude: the element is loudest at
// 50% duty and quieter as the duty moves away from 50% (shorter pulses deliver
// less energy). We approximate volume that way; it is far less linear than the
// I2S path's true amplitude scaling, but adequate for a monitor/practice tone.
// ---------------------------------------------------------------------------

static constexpr int     LEDC_CH    = 0;            // free (no other LEDC users)
static constexpr uint8_t LEDC_BITS  = 10;           // duty range 0..1023
static constexpr uint32_t FULL_DUTY = (1u << LEDC_BITS) / 2;  // 50% = loudest

static volatile bool s_on    = false;
static volatile bool s_muted = false;
static volatile bool s_alert = false;               // instructor alert overrides mute
static uint32_t      s_duty  = FULL_DUTY;           // duty applied while "on"

static inline void apply() {
    ledcWrite(LEDC_CH, (s_alert || (s_on && !s_muted)) ? s_duty : 0);
}

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    ledcSetup(LEDC_CH, freq_hz, LEDC_BITS);
    ledcAttachPin(PIN_SIDETONE, LEDC_CH);
    ledcWrite(LEDC_CH, 0);                           // start silent
}

void sidetone_set_volume(uint8_t vol) {
    // Map 0..255 onto a faint floor .. 50% duty (loudest for a square drive).
    s_duty = (uint32_t)FULL_DUTY * (vol + 1) / 256;
    apply();
}

void sidetone_set_level(uint8_t units) {
    if (units < 1)  units = 1;
    if (units > 32) units = 32;
    s_duty = (uint32_t)FULL_DUTY * units / 32;       // 32 -> full 50% duty
    apply();
}

void sidetone_set_mute(bool m) { s_muted = m; apply(); }
void sidetone_alert(bool on)   { s_alert = on; apply(); }
void sidetone_on()  { s_on = true;  apply(); }
void sidetone_off() { s_on = false; apply(); }

#else  // !SIDETONE_BUZZER
// Heltec V4 / V3 path: an I2S stream into a MAX98357A class-D amp. The
// Cardputer ADV instead routes sidetone through its on-board ES8311 codec; see
// sidetone_cardputer.cpp.
#include <math.h>
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------------------------------------------------------------------------
// Sidetone generator (MAX98357A / I2S).
//
// The MAX98357A is a digital (I2S-input) class-D amp, so there is no PWM
// carrier and no RC low-pass to average — we hand it 16-bit PCM samples
// directly. A dedicated feeder task free-runs the I2S DMA: it traces a sine
// (DDS) at the configured tone frequency, scales each sample by the volume
// gain, and writes interleaved L/R frames. When the tone is off or muted it
// writes silence instead of stopping the clock, so key down/up never pops and
// carries no setup latency.
//
// Because volume is now a true sample *amplitude* (not a PWM duty cycle), the
// timbre is identical at every level — this is the clean RSSI->loudness
// gradient the old PWM-sine path was reaching for, now trivial over I2S.
//
// MAX98357A wiring expectation (see docs/components/max98357a.md): SD_MODE tied
// high for the default (L+R)/2 mix, so we duplicate each sample into both slots
// and the amp reproduces it at full amplitude.
// ---------------------------------------------------------------------------

static constexpr i2s_port_t I2S_PORT     = I2S_NUM_0;
static constexpr uint32_t   SAMPLE_RATE  = 24000;   // ample headroom for a 750 Hz tone
static constexpr int        BLOCK_FRAMES = 128;     // stereo frames per i2s_write

// 256-entry signed 16-bit sine, indexed by the top 8 bits of the DDS phase.
static int16_t           s_sine[256];
static volatile uint32_t s_phase     = 0;
static volatile uint32_t s_phase_inc = 0;           // Q32 DDS step per sample
// Amplitude gain in Q15 (32768 = full swing). Driven by sidetone_set_volume();
// nothing changes it at runtime today, so this default sets the playback level.
// Full swing into the 4 ohm speaker is uncomfortably loud, so default ~ -12 dB
// (a quarter of full amplitude). Raise toward 32768 for more level.
static constexpr int32_t DEFAULT_GAIN_Q15 = 8192;
static volatile int32_t  s_gain_q15  = DEFAULT_GAIN_Q15;
static volatile bool     s_on        = false;
static volatile bool     s_muted     = false;
static volatile bool     s_alert     = false;       // instructor alert overrides mute
static TaskHandle_t      s_task       = nullptr;

// Raised-cosine amplitude envelope. Key on/off ramps the level in/out over a few
// ms instead of hard-gating the tone. A hard cut steps from a mid-cycle sample
// straight to zero; the step's click energy depends on the random cutoff phase,
// which made some elements sound much louder than others.
//
// The ramp shape must be a raised cosine, not a straight line: a *linear* ramp
// still has sharp corners (slope discontinuities) at its two ends, and those
// corners splatter spectrally — heard as a "chirp"/click, loudest on the first
// element after a silence (a word start), where nothing masks it. A raised cosine
// (0.5 - 0.5*cos) has zero slope at both endpoints, so there are no corners and
// no splatter. s_ramp[] holds that curve in Q15; s_env_pos walks it 0..ENV_RAMP.
static constexpr int ENV_RAMP = SAMPLE_RATE / 125;   // ~8 ms ramp, in samples
static int16_t       s_ramp[ENV_RAMP + 1];           // raised-cosine, 0..32767 (Q15)
static int           s_env_pos = 0;                  // current position, 0..ENV_RAMP

// First-tone soft-start (field note §1). The amp reboot-loops on the *first*
// full-amplitude drive out of silence — a one-shot, load-dependent turn-on
// transient of the class-D output stage (first-tone-only, probabilistic, gone
// once any tone succeeds, present only with the amp connected). The 8 ms key
// envelope above is far too fast to ease that first drive. So on the first tone
// out of silence, ramp a one-time amplitude ceiling from 0 up to full over
// ~250 ms, letting the amp slew gently into its first high-amplitude output
// instead of stepping to it.
//
// "Out of silence" means once per *mute cycle*, not just once per power cycle:
// it arms at power-up and RE-ARMS whenever the unit is muted, so the next unmute
// eases the amp back in too (the reboot was first seen on an over-the-air
// UNMUTE). It only advances while the tone is intended on, so it conditions the
// amp on the first real element after each unmute, whenever that element fires;
// after it completes, normal per-element keying uses the fast envelope alone.
static constexpr int     SOFTSTART_SAMPLES = SAMPLE_RATE / 4;   // ~250 ms
static volatile bool     s_softstart_done  = false;
static volatile int      s_softstart_pos   = 0;      // 0..SOFTSTART_SAMPLES, one-shot per cycle

static void feeder(void*) {
    static int16_t buf[BLOCK_FRAMES * 2];           // interleaved L, R
    for (;;) {
        // Sample the gate once per block; the per-sample envelope walk below makes
        // the actual on/off smooth and sample-accurate within the ramp.
        int target_pos = (s_alert || (s_on && !s_muted)) ? ENV_RAMP : 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            if      (s_env_pos < target_pos) s_env_pos++;   // attack along the curve
            else if (s_env_pos > target_pos) s_env_pos--;   // release along the curve
            int32_t env = s_ramp[s_env_pos];

            // One-time amplitude soft-start: advance only while the tone is meant
            // to be on, and only until it reaches full. Linear is fine here — over
            // ~250 ms the slope is far too gentle to splatter (unlike the 8 ms key
            // edge, which needs the raised cosine). Past full it's a constant 1.0.
            if (!s_softstart_done) {
                if (target_pos > 0 && s_softstart_pos < SOFTSTART_SAMPLES) s_softstart_pos++;
                if (s_softstart_pos >= SOFTSTART_SAMPLES) s_softstart_done = true;
            }
            int32_t ss = s_softstart_done
                             ? 32768
                             : (int32_t)((int64_t)s_softstart_pos * 32768 / SOFTSTART_SAMPLES);

            s_phase += s_phase_inc;                  // DDS free-runs; envelope, not
                                                     // phase, gives a clickless edge
            int32_t s = s_sine[(uint8_t)(s_phase >> 24)];
            int32_t v = (s * s_gain_q15) >> 15;      // volume
            v = (v * env) >> 15;                      // raised-cosine envelope
            v = (v * ss) >> 15;                       // one-time first-tone soft-start
            buf[2 * i] = (int16_t)v;                 // L
            buf[2 * i + 1] = (int16_t)v;             // R (SD_MODE high => (L+R)/2 = v)
        }
        size_t wrote;
        i2s_write(I2S_PORT, buf, sizeof(buf), &wrote, portMAX_DELAY);
    }
}

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    for (int i = 0; i < 256; i++)
        s_sine[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * (float)M_PI * i / 256.0f));
    for (int i = 0; i <= ENV_RAMP; i++)
        s_ramp[i] = (int16_t)lrintf(32767.0f * 0.5f *
                                    (1.0f - cosf((float)M_PI * i / ENV_RAMP)));
    s_phase_inc = (uint32_t)(((uint64_t)freq_hz << 32) / SAMPLE_RATE);

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;   // stereo frames
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 6;
    cfg.dma_buf_len          = BLOCK_FRAMES;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCLK;
    pins.data_out_num = PIN_I2S_DIN;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);

    // Pin to core 0; the radio/main loop runs on core 1. Priority above idle but
    // below time-critical work — i2s_write blocks on DMA space, so it self-paces.
    xTaskCreatePinnedToCore(feeder, "sidetone", 4096, nullptr, 2, &s_task, 0);
}

void sidetone_set_volume(uint8_t vol) {
    // Map 0..255 onto the amplitude gain with an exponential (dB-linear) curve so
    // equal input steps give equal *perceived* loudness steps.
    constexpr float GAIN_MAX = 32768.0f;   // full swing
    constexpr float GAIN_MIN = 512.0f;     // faint floor (~ -36 dB)
    float n = (float)vol / 255.0f;
    s_gain_q15 = (int32_t)(GAIN_MIN * powf(GAIN_MAX / GAIN_MIN, n) + 0.5f);
}

void sidetone_set_level(uint8_t units) {
    if (units < 1)  units = 1;
    if (units > 32) units = 32;                     // 32 * 1024 = 32768 = full swing
    s_gain_q15 = (int32_t)units * 1024;
}

void sidetone_set_mute(bool m) {
    if (m && !s_muted) {                 // entering mute: re-arm the first-tone
        s_softstart_pos  = 0;            // soft-start so the next unmute eases the
        s_softstart_done = false;        // amp back in from silence (field note §1)
    }
    s_muted = m;                          // feeder writes silence while muted
}
void sidetone_alert(bool on)   { s_alert = on; }    // feeder forces tone on while set

void sidetone_on()  { s_on = true; }   // the envelope ramp gives the clickless edge
void sidetone_off() { s_on = false; }

#endif // SIDETONE_BUZZER
#endif // DEVICE_HELTEC_V4 || DEVICE_HELTEC_V3
