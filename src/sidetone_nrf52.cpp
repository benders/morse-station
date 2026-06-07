// sidetone_nrf52.cpp — RAK4631 (nRF52840) sidetone backend.
//
// 16-bit PCM streamed over the nRF52840 I2S peripheral into a MAX98357A class-D
// amp -> small speaker. This is the same generator design as the Heltec I2S path
// (sidetone.cpp): a DDS sine scaled by a volume gain, with a raised-cosine
// attack/release envelope so key down/up is click-free; the difference is purely
// the transport — the Adafruit nRF52 core ships no Arduino I2S wrapper, so we
// drive the I2S registers directly (HAL-free, register-level) and ping-pong two
// EasyDMA buffers from a FreeRTOS feeder task.
//
// Hardware tolerance: I2S *master TX* is push-only — it clocks SCK/LRCK and
// shifts SDOUT with no handshake and no readback. So if the MAX98357A is absent
// or unwired (as on a bare bring-up board), init still completes and boot is
// unaffected; the pins simply toggle into nothing. (Contrast the OLED I2C path,
// where a stuck slave could hang the bus — see display.cpp.)
//
// Pin map / wiring: PIN_I2S_BCLK / _LRCLK / _DIN come from platformio.ini
// ([env:rak4631] build_flags), mirrored by pins.h fallbacks. They land on the
// RAK19007 J10/J11 solder headers. Tie the amp's GAIN pin to VIN (a floating
// GAIN latches off stray voltage -> intermittent distortion, the exact bug seen
// on the Heltec build; see docs/components/max98357a.md).
//
// Guarded to compile only for the RAK4631 target. The ESP32 I2S sibling
// (sidetone.cpp) is guarded to Heltec V4/V3; sidetone_cardputer.cpp self-guards
// to the Cardputer ADV.

#if defined(DEVICE_RAK4631)

#include "sidetone.h"
#include "pins.h"
#include <Arduino.h>
#include <nrf.h>
#include <math.h>
#include <FreeRTOS.h>
#include <task.h>

namespace {

// MCK = 32 MHz / 15 = 2.1333 MHz; LRCK = MCK / 128 = 16.667 kHz. Ample headroom
// for a 600 Hz tone and a multiple of the 256-entry sine's harmonics-free reach.
constexpr uint32_t SAMPLE_RATE = 16667;
constexpr int      FRAMES      = 256;     // stereo frames per DMA buffer; one
                                          // 16-bit L+R pair packs into one word,
                                          // so MAXCNT (in 32-bit words) == FRAMES.
                                          // ~15 ms/buffer => relaxed feeder cadence.

// 256-entry signed 16-bit sine, indexed by the top 8 bits of the DDS phase.
int16_t           s_sine[256];
volatile uint32_t s_phase     = 0;
volatile uint32_t s_phase_inc = 0;        // Q32 DDS step per sample

// Amplitude gain in Q15 (32768 = full swing). Default ~ -12 dB; full swing into
// a small speaker is uncomfortably loud. Driven by sidetone_set_level/_volume().
constexpr int32_t DEFAULT_GAIN_Q15 = 8192;
volatile int32_t  s_gain_q15 = DEFAULT_GAIN_Q15;
volatile bool     s_on       = false;
volatile bool     s_muted    = false;

// Raised-cosine key envelope (same rationale as sidetone.cpp): zero slope at both
// ends => no click/chirp on the first element after silence. ~8 ms ramp.
constexpr int ENV_RAMP = SAMPLE_RATE / 125;
int16_t       s_ramp[ENV_RAMP + 1];       // raised-cosine, 0..32767 (Q15)
int           s_env_pos = 0;              // current position, 0..ENV_RAMP

// Two EasyDMA TX buffers (must live in RAM, 32-bit aligned). Each word holds the
// sample in both halves (L and R identical) so the output is mono regardless of
// the amp's L/R slot selection.
__attribute__((aligned(4))) uint32_t s_buf[2][FRAMES];

// Fill one DMA buffer with the next FRAMES samples (DDS + volume + envelope).
void fill_buffer(uint32_t* buf) {
    const int target = (s_on && !s_muted) ? ENV_RAMP : 0;
    for (int i = 0; i < FRAMES; i++) {
        if      (s_env_pos < target) s_env_pos++;   // attack along the curve
        else if (s_env_pos > target) s_env_pos--;   // release along the curve
        int32_t env = s_ramp[s_env_pos];

        s_phase += s_phase_inc;                      // DDS free-runs; envelope,
                                                     // not phase, gives the edge
        int32_t s = s_sine[(uint8_t)(s_phase >> 24)];
        int32_t v = (s * s_gain_q15) >> 15;          // volume
        v = (v * env) >> 15;                          // raised-cosine envelope
        uint16_t u = (uint16_t)(int16_t)v;
        buf[i] = ((uint32_t)u << 16) | u;            // L and R both = v
    }
}

TaskHandle_t s_task = nullptr;

// Feeder task: each time the peripheral latches a buffer (EVENTS_TXPTRUPD), refill
// the *other* buffer and stage it as the next TXD.PTR. The just-latched buffer is
// still being transmitted, so we never write the in-flight one. Polling at 1 ms
// against a ~15 ms buffer leaves a wide margin; a missed deadline only replays a
// buffer (a glitch), never crashes.
void feeder(void*) {
    int toggle = 1;
    for (;;) {
        if (NRF_I2S->EVENTS_TXPTRUPD) {
            NRF_I2S->EVENTS_TXPTRUPD = 0;
            fill_buffer(s_buf[toggle]);
            NRF_I2S->TXD.PTR = (uint32_t)s_buf[toggle];
            toggle ^= 1;
        } else {
            vTaskDelay(1);
        }
    }
}

} // namespace

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    for (int i = 0; i < 256; i++)
        s_sine[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * (float)M_PI * i / 256.0f));
    for (int i = 0; i <= ENV_RAMP; i++)
        s_ramp[i] = (int16_t)lrintf(32767.0f * 0.5f *
                                    (1.0f - cosf((float)M_PI * i / ENV_RAMP)));
    s_phase_inc = (uint32_t)(((uint64_t)freq_hz << 32) / SAMPLE_RATE);

    // Start silent; both buffers cleared so the first frames out are silence.
    s_on = false;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < FRAMES; i++) s_buf[b][i] = 0;

    // Drive the three signal pins as outputs before handing them to the I2S GPIO
    // matrix (avoids a momentary float/glitch as routing connects).
    pinMode(PIN_I2S_BCLK,  OUTPUT);
    pinMode(PIN_I2S_LRCLK, OUTPUT);
    pinMode(PIN_I2S_DIN,   OUTPUT);

    // GPIO matrix routing. PSEL value is the absolute nRF GPIO number
    // (port<<5 | pin); this variant maps Arduino pin == absolute GPIO, so
    // g_ADigitalPinMap[] yields that number directly. bit31 = 0 connects the pin;
    // 0x80000000 leaves it disconnected. MCK is not routed — the MAX98357A
    // self-clocks from BCLK/LRC and needs no master clock; SDIN is unused (TX).
    NRF_I2S->PSEL.MCK   = 0x80000000;
    NRF_I2S->PSEL.SCK   = g_ADigitalPinMap[PIN_I2S_BCLK];
    NRF_I2S->PSEL.LRCK  = g_ADigitalPinMap[PIN_I2S_LRCLK];
    NRF_I2S->PSEL.SDOUT = g_ADigitalPinMap[PIN_I2S_DIN];
    NRF_I2S->PSEL.SDIN  = 0x80000000;

    NRF_I2S->CONFIG.MODE     = I2S_CONFIG_MODE_MODE_Master         << I2S_CONFIG_MODE_MODE_Pos;
    NRF_I2S->CONFIG.RXEN     = I2S_CONFIG_RXEN_RXEN_Disabled       << I2S_CONFIG_RXEN_RXEN_Pos;
    NRF_I2S->CONFIG.TXEN     = I2S_CONFIG_TXEN_TXEN_Enabled        << I2S_CONFIG_TXEN_TXEN_Pos;
    NRF_I2S->CONFIG.MCKEN    = I2S_CONFIG_MCKEN_MCKEN_Enabled      << I2S_CONFIG_MCKEN_MCKEN_Pos;
    NRF_I2S->CONFIG.MCKFREQ  = I2S_CONFIG_MCKFREQ_MCKFREQ_32MDIV15 << I2S_CONFIG_MCKFREQ_MCKFREQ_Pos;
    NRF_I2S->CONFIG.RATIO    = I2S_CONFIG_RATIO_RATIO_128X         << I2S_CONFIG_RATIO_RATIO_Pos;
    NRF_I2S->CONFIG.SWIDTH   = I2S_CONFIG_SWIDTH_SWIDTH_16Bit      << I2S_CONFIG_SWIDTH_SWIDTH_Pos;
    NRF_I2S->CONFIG.ALIGN    = I2S_CONFIG_ALIGN_ALIGN_Left         << I2S_CONFIG_ALIGN_ALIGN_Pos;
    NRF_I2S->CONFIG.FORMAT   = I2S_CONFIG_FORMAT_FORMAT_I2S        << I2S_CONFIG_FORMAT_FORMAT_Pos;
    NRF_I2S->CONFIG.CHANNELS = I2S_CONFIG_CHANNELS_CHANNELS_Stereo << I2S_CONFIG_CHANNELS_CHANNELS_Pos;

    NRF_I2S->RXTXD.MAXCNT = FRAMES;            // buffer size in 32-bit words

    NRF_I2S->TXD.PTR = (uint32_t)s_buf[0];
    NRF_I2S->ENABLE  = 1;
    NRF_I2S->TASKS_START = 1;

    // 512-word (2 KB) stack — the feeder does only integer work (the float sine/
    // ramp tables are built above, on the caller's stack). Modest priority: it
    // self-paces on the buffer cadence and makes no SoftDevice calls.
    xTaskCreate(feeder, "sidetone", 512, nullptr, tskIDLE_PRIORITY + 2, &s_task);
}

void sidetone_set_volume(uint8_t vol) {
    // Map 0..255 onto the amplitude gain with an exponential (dB-linear) curve so
    // equal input steps give equal perceived-loudness steps (matches sidetone.cpp).
    constexpr float GAIN_MAX = 32768.0f;   // full swing
    constexpr float GAIN_MIN = 512.0f;     // faint floor (~ -36 dB)
    float n = (float)vol / 255.0f;
    s_gain_q15 = (int32_t)(GAIN_MIN * powf(GAIN_MAX / GAIN_MIN, n) + 0.5f);
}

void sidetone_set_level(uint8_t units) {
    if (units < 1)  units = 1;
    if (units > 32) units = 32;            // 32 * 1024 = 32768 = full swing
    s_gain_q15 = (int32_t)units * 1024;
}

void sidetone_set_mute(bool m) { s_muted = m; }   // feeder writes silence while muted

void sidetone_on()  { s_on = true; }   // the envelope ramp gives the clickless edge
void sidetone_off() { s_on = false; }

#endif // DEVICE_RAK4631
