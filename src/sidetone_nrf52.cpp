// sidetone_nrf52.cpp — RAK4631 (nRF52840) sidetone backend.
//
// Two transports, selected at *build time* (see platformio.ini [env:rak4631]):
//
//   default            -> I2S into a MAX98357A class-D amp (digital, 16-bit PCM).
//   -DSIDETONE_PAM8403 -> PWM into an RC low-pass into a PAM8403 analog amp.
//
// Both share the exact same generator design as the Heltec I2S path
// (sidetone.cpp): a DDS sine scaled by a volume gain, with a raised-cosine
// attack/release envelope so key down/up is click-free. The volume/level/mute
// knobs are byte-for-byte identical between the two paths; only the *transport*
// (how a sample becomes a pin waveform) differs, and that is the only part the
// #if defined(SIDETONE_PAM8403) split below covers.
//
// I2S path: the Adafruit nRF52 core ships no Arduino I2S wrapper, so we drive
// the I2S registers directly (HAL-free) and ping-pong two EasyDMA buffers from a
// FreeRTOS feeder. MCK is not routed — the MAX98357A self-clocks from BCLK/LRC.
//
// PWM path: the nRF52840 PWM peripheral free-runs a high-frequency carrier
// (62.5 kHz) and EasyDMA-streams a sequence of compare values that trace the
// sine as a varying duty cycle (a "pseudo-DAC"). An external RC low-pass
// (~1k + 100nF, corner ~1.6 kHz) averages the carrier back into an analog sine
// for the PAM8403's line input. Idle/mute parks at 50% duty (mid-rail) so the
// filtered output sits at a DC midpoint with no AC — silent, no pop. Two
// sequence buffers ping-pong via LOOP mode (SHORT LOOPSDONE->SEQSTART0) and the
// same FreeRTOS feeder pattern as the I2S path.
//
// Hardware tolerance: both transports are output-only and push-only — they
// clock the pins with no handshake and no readback. So if the amp is absent or
// unwired (as on a bare bring-up board), init still completes and boot is
// unaffected; the pins simply toggle into nothing. (Contrast the OLED I2C path,
// where a stuck slave could hang the bus — see display.cpp.)
//
// Pin map / wiring comes from platformio.ini ([env:rak4631] build_flags),
// mirrored by pins.h fallbacks. I2S: PIN_I2S_BCLK/_LRCLK/_DIN. PWM: a single
// PIN_SIDETONE_PWM. Both land on the RAK19007 J10/J11 solder headers. For the
// MAX98357A tie GAIN to VIN (a floating GAIN -> intermittent distortion, the
// exact bug seen on the Heltec build; see docs/components/max98357a.md).
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

#if defined(SIDETONE_PAM8403)
// PWM carrier = 16 MHz / COUNTERTOP = 62.5 kHz (well above audio, easy to RC
// filter). REFRESH=1 holds each compare value for 2 carrier periods, so the
// audio sample rate is carrier / (REFRESH+1) = 31.25 kHz. Duty is 9-bit-ish:
// 0..PWM_TOP, with PWM_MID = 50% = the AC zero crossing (DC mid-rail).
constexpr uint32_t SAMPLE_RATE = 31250;
constexpr int      FRAMES      = 256;     // compare values per DMA sequence
constexpr uint16_t PWM_TOP     = 256;     // COUNTERTOP
constexpr uint16_t PWM_MID     = PWM_TOP / 2;
constexpr uint32_t PWM_REFRESH = 1;       // extra carrier periods per sample
#else
// MCK = 32 MHz / 15 = 2.1333 MHz; LRCK = MCK / 128 = 16.667 kHz. Ample headroom
// for a 600 Hz tone and a multiple of the 256-entry sine's harmonics-free reach.
constexpr uint32_t SAMPLE_RATE = 16667;
constexpr int      FRAMES      = 256;     // stereo frames per DMA buffer; one
                                          // 16-bit L+R pair packs into one word,
                                          // so MAXCNT (in 32-bit words) == FRAMES.
                                          // ~15 ms/buffer => relaxed feeder cadence.
#endif

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

#if defined(SIDETONE_PAM8403)
// ---- PWM (PAM8403) transport ----------------------------------------------

// Two EasyDMA sequence buffers (must live in RAM, 32-bit aligned). Each entry is
// one 16-bit PWM compare value; bit15 = polarity (left 0 => duty = value/TOP).
__attribute__((aligned(4))) uint16_t s_buf[2][FRAMES];

// Fill one sequence with the next FRAMES samples, mapped to PWM duty around the
// mid-rail. Idle/mute relaxes the envelope to 0 => every sample is PWM_MID (50%
// duty) => the RC-filtered output is flat DC => silent, no DC step (no pop).
void fill_buffer(uint16_t* buf) {
    const int target = (s_on && !s_muted) ? ENV_RAMP : 0;
    for (int i = 0; i < FRAMES; i++) {
        if      (s_env_pos < target) s_env_pos++;   // attack along the curve
        else if (s_env_pos > target) s_env_pos--;   // release along the curve
        int32_t env = s_ramp[s_env_pos];

        s_phase += s_phase_inc;                      // DDS free-runs; envelope,
                                                     // not phase, gives the edge
        int32_t s = s_sine[(uint8_t)(s_phase >> 24)];
        int32_t v = (s * s_gain_q15) >> 15;          // volume   (-32768..32767)
        v = (v * env) >> 15;                          // raised-cosine envelope
        int32_t duty = PWM_MID + ((v * PWM_MID) >> 15);   // center on mid-rail
        if (duty < 0)            duty = 0;
        if (duty > PWM_TOP)      duty = PWM_TOP;
        buf[i] = (uint16_t)duty;                      // bit15=0 => duty/TOP
    }
}
#else
// ---- I2S (MAX98357A) transport --------------------------------------------

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
#endif

TaskHandle_t s_task = nullptr;

#if defined(SIDETONE_PAM8403)
// Feeder task: LOOP mode plays SEQ0 then SEQ1 then (via the LOOPSDONE->SEQSTART0
// short) restarts, forever. EVENTS_SEQEND[n] fires when sequence n has been
// fully shifted out, so we refill the *just-finished* buffer while the other one
// plays — never the in-flight one. Polling at 1 ms against an ~8 ms sequence
// leaves a wide margin; a missed deadline only replays a buffer (a glitch).
void feeder(void*) {
    for (;;) {
        bool did = false;
        if (NRF_PWM0->EVENTS_SEQEND[0]) {
            NRF_PWM0->EVENTS_SEQEND[0] = 0;
            fill_buffer(s_buf[0]);
            did = true;
        }
        if (NRF_PWM0->EVENTS_SEQEND[1]) {
            NRF_PWM0->EVENTS_SEQEND[1] = 0;
            fill_buffer(s_buf[1]);
            did = true;
        }
        if (!did) vTaskDelay(1);
    }
}
#else
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
#endif

} // namespace

void sidetone_init(int /*gpio*/, uint32_t freq_hz) {
    for (int i = 0; i < 256; i++)
        s_sine[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * (float)M_PI * i / 256.0f));
    for (int i = 0; i <= ENV_RAMP; i++)
        s_ramp[i] = (int16_t)lrintf(32767.0f * 0.5f *
                                    (1.0f - cosf((float)M_PI * i / ENV_RAMP)));
    s_phase_inc = (uint32_t)(((uint64_t)freq_hz << 32) / SAMPLE_RATE);

    // Start silent; both buffers primed so the first samples out are silence.
    s_on = false;

#if defined(SIDETONE_PAM8403)
    // Prime both sequences at mid-rail (50% duty = filtered DC = silent).
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < FRAMES; i++) s_buf[b][i] = PWM_MID;

    // Drive the analog pin as an output before handing it to the PWM peripheral
    // (avoids a momentary float/glitch as routing connects).
    pinMode(PIN_SIDETONE, OUTPUT);
    digitalWrite(PIN_SIDETONE, LOW);

    // Only channel 0 is used; PSEL value is the absolute nRF GPIO number, which
    // this variant maps as Arduino pin == absolute GPIO (g_ADigitalPinMap[]).
    // bit31 = 0 connects; 0x80000000 leaves the channel disconnected.
    NRF_PWM0->PSEL.OUT[0] = g_ADigitalPinMap[PIN_SIDETONE];
    NRF_PWM0->PSEL.OUT[1] = 0x80000000;
    NRF_PWM0->PSEL.OUT[2] = 0x80000000;
    NRF_PWM0->PSEL.OUT[3] = 0x80000000;

    NRF_PWM0->ENABLE     = PWM_ENABLE_ENABLE_Enabled << PWM_ENABLE_ENABLE_Pos;
    NRF_PWM0->MODE       = PWM_MODE_UPDOWN_Up        << PWM_MODE_UPDOWN_Pos;
    NRF_PWM0->PRESCALER  = PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos;
    NRF_PWM0->COUNTERTOP = PWM_TOP;
    NRF_PWM0->LOOP       = 1;   // 1 loop iteration (SEQ0+SEQ1), then LOOPSDONE
    NRF_PWM0->DECODER    = (PWM_DECODER_LOAD_Common      << PWM_DECODER_LOAD_Pos) |
                           (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);

    NRF_PWM0->SEQ[0].PTR      = (uint32_t)s_buf[0];
    NRF_PWM0->SEQ[0].CNT      = FRAMES;
    NRF_PWM0->SEQ[0].REFRESH  = PWM_REFRESH;
    NRF_PWM0->SEQ[0].ENDDELAY = 0;
    NRF_PWM0->SEQ[1].PTR      = (uint32_t)s_buf[1];
    NRF_PWM0->SEQ[1].CNT      = FRAMES;
    NRF_PWM0->SEQ[1].REFRESH  = PWM_REFRESH;
    NRF_PWM0->SEQ[1].ENDDELAY = 0;

    // LOOPSDONE -> SEQSTART0 makes playback restart automatically: SEQ0, SEQ1,
    // SEQ0, SEQ1, ... forever, with no CPU in the loop (just buffer refills).
    NRF_PWM0->SHORTS = PWM_SHORTS_LOOPSDONE_SEQSTART0_Enabled
                       << PWM_SHORTS_LOOPSDONE_SEQSTART0_Pos;

    NRF_PWM0->EVENTS_SEQEND[0] = 0;
    NRF_PWM0->EVENTS_SEQEND[1] = 0;
    NRF_PWM0->TASKS_SEQSTART[0] = 1;
#else
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
#endif

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
