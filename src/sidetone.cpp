#include "sidetone.h"
#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)
// Heltec V4 / V3 path: an I2S stream into a MAX98357A class-D amp. The
// Cardputer ADV instead routes sidetone through its on-board ES8311 codec; see
// sidetone_cardputer.cpp.
#include <Arduino.h>
#include <math.h>
#include "pins.h"
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

static void feeder(void*) {
    static int16_t buf[BLOCK_FRAMES * 2];           // interleaved L, R
    for (;;) {
        // Sample the gate once per block; the per-sample envelope walk below makes
        // the actual on/off smooth and sample-accurate within the ramp.
        int target_pos = (s_on && !s_muted) ? ENV_RAMP : 0;
        for (int i = 0; i < BLOCK_FRAMES; i++) {
            if      (s_env_pos < target_pos) s_env_pos++;   // attack along the curve
            else if (s_env_pos > target_pos) s_env_pos--;   // release along the curve
            int32_t env = s_ramp[s_env_pos];

            s_phase += s_phase_inc;                  // DDS free-runs; envelope, not
                                                     // phase, gives a clickless edge
            int32_t s = s_sine[(uint8_t)(s_phase >> 24)];
            int32_t v = (s * s_gain_q15) >> 15;      // volume
            v = (v * env) >> 15;                      // raised-cosine envelope
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

void sidetone_set_mute(bool m) { s_muted = m; }     // feeder writes silence while muted

void sidetone_on()  { s_on = true; }   // the envelope ramp gives the clickless edge
void sidetone_off() { s_on = false; }

#endif // DEVICE_HELTEC_V4 || DEVICE_HELTEC_V3
