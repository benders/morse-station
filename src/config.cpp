#include "config.h"
#include "kv.h"
#include "platform.h"
#include <Arduino.h>
#include <string.h>

namespace {
kv::Store prefs;
uint8_t cached_id = 1;
char    cached_call[config::CALLSIGN_MAX + 1] = "N0CALL";
char    cached_msg[config::FOX_MSG_MAX + 1]   = "DE N0CALL FOX NEAR THE BIG OAK BY THE LAKE";
uint8_t cached_wpm      = 15;     // overall (effective) speed
uint8_t cached_char_wpm = 18;     // Farnsworth character speed
uint8_t cached_msgmode  = 1;      // 0=keyed (edge Morse), 1=text (TextMsg frame, default)
uint8_t cached_boot_mode = 0;     // last-selected boot mode (Mode enum, 0=Hunter)
uint8_t cached_fox_pwr_idx = 0;   // last-selected fox TX power level (PWR_LEVELS index, 0=LO)
bool    cached_muted = false;     // sidetone mute (silent node), persisted
bool    cached_lna   = true;      // V4.3 FEM RX LNA in path (CTX select), persisted
uint32_t cached_rx_bw_dhz = 234;  // FSK RX bandwidth in deci-kHz (234 = 23.4 kHz),
                                  // mirrors radio.cpp RX_BW_KHZ default
uint8_t cached_ctrl_seq = 0;      // instructor control-packet seq (persisted)
uint8_t cached_volume = 8;        // sidetone level in GAIN_Q15/1024 units (8 -> 8192)
uint8_t cached_fox_id = 0;        // session fox station id for the instructor menu (0=unset)

// Bitmask of cached_* fields with a flash write pending. Setters only set the
// bit (cheap, RAM-only, safe to call from anywhere incl. a radio RX handler);
// flush() does the actual prefs.begin/put/end and is called once per loop().
enum : uint16_t {
    D_ID = 1u << 0, D_CALL = 1u << 1, D_MSG = 1u << 2, D_WPM = 1u << 3,
    D_CWPM = 1u << 4, D_MMODE = 1u << 5, D_BMODE = 1u << 6, D_FPWR = 1u << 7,
    D_MUTE = 1u << 8, D_LNA = 1u << 9, D_RXBW = 1u << 10, D_CSEQ = 1u << 11,
    D_VOL = 1u << 12, D_FID = 1u << 13,
};
uint16_t dirty = 0;

// Compile-time platform name — always correct for the firmware variant. This is
// the default board model; a more specific model (e.g. a Heltec V4 sub-rev that
// is NOT electrically distinguishable at runtime) can be pinned per unit in NVS.
#if defined(DEVICE_HELTEC_V4)
constexpr const char* DEF_MODEL = "heltec-v4";
#elif defined(DEVICE_HELTEC_V3)
constexpr const char* DEF_MODEL = "heltec-v3";
#elif defined(DEVICE_CARDPUTER_ADV)
constexpr const char* DEF_MODEL = "cardputer-adv";
#elif defined(DEVICE_RAK4631)
constexpr const char* DEF_MODEL = "rak4631";
#elif defined(DEVICE_WIO_TRACKER_L1)
constexpr const char* DEF_MODEL = "wio-tracker-l1";
#else
constexpr const char* DEF_MODEL = "unknown";
#endif

constexpr uint8_t WPM_MIN = 5;
constexpr uint8_t WPM_MAX = 40;
constexpr uint8_t VOL_MIN = 1;
constexpr uint8_t VOL_MAX = 32;   // 32 * 1024 = 32768 = full swing

constexpr const char* NS        = "morse";
constexpr const char* KEY_ID    = "station_id";
constexpr const char* KEY_CALL  = "callsign";
constexpr const char* KEY_MSG   = "fox_msg";
constexpr const char* KEY_WPM   = "wpm";
constexpr const char* KEY_CWPM  = "char_wpm";
constexpr const char* KEY_MMODE = "msgmode";
constexpr const char* KEY_BMODE = "boot_mode";
constexpr const char* KEY_FPWR  = "fox_pwr_idx";
constexpr const char* KEY_MUTE  = "muted";
constexpr const char* KEY_LNA   = "lna";
constexpr const char* KEY_RXBW  = "rx_bw_dhz";
constexpr const char* KEY_CSEQ  = "ctrl_seq";
constexpr const char* KEY_VOL   = "volume";
constexpr const char* KEY_FID   = "fox_id";

uint8_t clamp_u8(int v, uint8_t lo, uint8_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (uint8_t)v;
}

}

namespace config {

void begin() {
    prefs.begin(NS, false);

    // Default station id from the platform's stable device identifier (eFuse
    // MAC on ESP32, FICR DEVICEID on nRF52) folded into 1..254.  If an id has
    // been provisioned, use that instead.
    cached_id = prefs.isKey(KEY_ID) ? prefs.getUChar(KEY_ID, 1)
                                    : platform::unique_id_byte();

    if (prefs.isKey(KEY_CALL))
        prefs.getString(KEY_CALL, cached_call, sizeof(cached_call));
    if (prefs.isKey(KEY_MSG))
        prefs.getString(KEY_MSG, cached_msg, sizeof(cached_msg));

    if (prefs.isKey(KEY_WPM))
        cached_wpm = clamp_u8(prefs.getUChar(KEY_WPM, cached_wpm), WPM_MIN, WPM_MAX);
    if (prefs.isKey(KEY_CWPM))
        cached_char_wpm = clamp_u8(prefs.getUChar(KEY_CWPM, cached_char_wpm),
                                   cached_wpm, WPM_MAX);
    if (cached_char_wpm < cached_wpm) cached_char_wpm = cached_wpm;

    if (prefs.isKey(KEY_MMODE))
        cached_msgmode = prefs.getUChar(KEY_MMODE, cached_msgmode) ? 1 : 0;

    if (prefs.isKey(KEY_BMODE))
        cached_boot_mode = prefs.getUChar(KEY_BMODE, cached_boot_mode);

    if (prefs.isKey(KEY_FPWR))
        cached_fox_pwr_idx = prefs.getUChar(KEY_FPWR, cached_fox_pwr_idx);

    if (prefs.isKey(KEY_MUTE))
        cached_muted = prefs.getBool(KEY_MUTE, cached_muted);

    if (prefs.isKey(KEY_LNA))
        cached_lna = prefs.getBool(KEY_LNA, cached_lna);

    if (prefs.isKey(KEY_RXBW))
        cached_rx_bw_dhz = prefs.getUInt(KEY_RXBW, cached_rx_bw_dhz);

    if (prefs.isKey(KEY_CSEQ))
        cached_ctrl_seq = prefs.getUChar(KEY_CSEQ, cached_ctrl_seq);

    if (prefs.isKey(KEY_VOL))
        cached_volume = clamp_u8(prefs.getUChar(KEY_VOL, cached_volume), VOL_MIN, VOL_MAX);

    if (prefs.isKey(KEY_FID))
        cached_fox_id = prefs.getUChar(KEY_FID, cached_fox_id);

    prefs.end();
}

uint8_t station_id() { return cached_id; }

void set_station_id(uint8_t id) {
    cached_id = id;
    dirty |= D_ID;
}

const char* callsign() { return cached_call; }

void set_callsign(const char* call) {
    if (!call) return;
    strlcpy(cached_call, call, sizeof(cached_call));
    dirty |= D_CALL;
}

const char* fox_message() { return cached_msg; }

void set_fox_message(const char* msg) {
    if (!msg) return;
    strlcpy(cached_msg, msg, sizeof(cached_msg));
    dirty |= D_MSG;
}

uint8_t wpm() { return cached_wpm; }

void set_wpm(uint8_t wpm) {
    cached_wpm = clamp_u8(wpm, WPM_MIN, WPM_MAX);
    if (cached_char_wpm < cached_wpm) cached_char_wpm = cached_wpm;  // keep C >= S
    dirty |= D_WPM | D_CWPM;
}

uint8_t char_wpm() { return cached_char_wpm; }

void set_char_wpm(uint8_t wpm) {
    cached_char_wpm = clamp_u8(wpm, cached_wpm, WPM_MAX);  // never below overall
    dirty |= D_CWPM;
}

uint8_t msgmode() { return cached_msgmode; }

void set_msgmode(uint8_t mode) {
    uint8_t m = mode ? 1 : 0;
    if (m == cached_msgmode) return;   // avoid a needless flash write
    cached_msgmode = m;
    dirty |= D_MMODE;
}

uint8_t boot_mode() { return cached_boot_mode; }

void set_boot_mode(uint8_t mode) {
    if (mode == cached_boot_mode) return;   // avoid a needless flash write
    cached_boot_mode = mode;
    dirty |= D_BMODE;
}

uint8_t fox_pwr_idx() { return cached_fox_pwr_idx; }

void set_fox_pwr_idx(uint8_t idx) {
    if (idx == cached_fox_pwr_idx) return;   // avoid a needless flash write
    cached_fox_pwr_idx = idx;
    dirty |= D_FPWR;
}

uint8_t volume() { return cached_volume; }

void set_volume(uint8_t units) {
    uint8_t v = clamp_u8(units, VOL_MIN, VOL_MAX);
    if (v == cached_volume) return;   // avoid a needless flash write
    cached_volume = v;
    dirty |= D_VOL;
}

const char* default_board_model() { return DEF_MODEL; }

bool muted() { return cached_muted; }

void set_muted(bool m) {
    if (m == cached_muted) return;   // avoid a needless flash write
    cached_muted = m;
    dirty |= D_MUTE;
}

bool lna() { return cached_lna; }

void set_lna(bool on) {
    if (on == cached_lna) return;    // avoid a needless flash write
    cached_lna = on;
    dirty |= D_LNA;
}

float rx_bw_khz() { return cached_rx_bw_dhz / 10.0f; }

void set_rx_bw_khz(float khz) {
    uint32_t dhz = (uint32_t)(khz * 10.0f + 0.5f);
    if (dhz == cached_rx_bw_dhz) return;    // avoid a needless flash write
    cached_rx_bw_dhz = dhz;
    dirty |= D_RXBW;
}

uint8_t ctrl_seq() { return cached_ctrl_seq; }

void set_ctrl_seq(uint8_t seq) {
    if (seq == cached_ctrl_seq) return;     // avoid a needless flash write
    cached_ctrl_seq = seq;
    dirty |= D_CSEQ;
}

uint8_t fox_id() { return cached_fox_id; }

void set_fox_id(uint8_t id) {
    if (id == cached_fox_id) return;        // avoid a needless flash write
    cached_fox_id = id;
    dirty |= D_FID;
}

void flush() {
    if (!dirty) return;   // common case: nothing pending, skip the flash session
    prefs.begin(NS, false);
    if (dirty & D_ID)    prefs.putUChar(KEY_ID, cached_id);
    if (dirty & D_CALL)  prefs.putString(KEY_CALL, cached_call);
    if (dirty & D_MSG)   prefs.putString(KEY_MSG, cached_msg);
    if (dirty & D_WPM)   prefs.putUChar(KEY_WPM, cached_wpm);
    if (dirty & D_CWPM)  prefs.putUChar(KEY_CWPM, cached_char_wpm);
    if (dirty & D_MMODE) prefs.putUChar(KEY_MMODE, cached_msgmode);
    if (dirty & D_BMODE) prefs.putUChar(KEY_BMODE, cached_boot_mode);
    if (dirty & D_FPWR)  prefs.putUChar(KEY_FPWR, cached_fox_pwr_idx);
    if (dirty & D_MUTE)  prefs.putBool(KEY_MUTE, cached_muted);
    if (dirty & D_LNA)   prefs.putBool(KEY_LNA, cached_lna);
    if (dirty & D_RXBW)  prefs.putUInt(KEY_RXBW, cached_rx_bw_dhz);
    if (dirty & D_CSEQ)  prefs.putUChar(KEY_CSEQ, cached_ctrl_seq);
    if (dirty & D_VOL)   prefs.putUChar(KEY_VOL, cached_volume);
    if (dirty & D_FID)   prefs.putUChar(KEY_FID, cached_fox_id);
    prefs.end();
    dirty = 0;
}

} // namespace config
