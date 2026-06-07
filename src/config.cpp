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
uint8_t cached_boot_mode = 0;     // last-selected boot mode (Mode enum, 0=Hunter)
uint8_t cached_fox_pwr_idx = 0;   // last-selected fox TX power level (PWR_LEVELS index, 0=LO)
bool    cached_muted = false;     // sidetone mute (silent node), persisted
uint8_t cached_volume = 8;        // sidetone level in GAIN_Q15/1024 units (8 -> 8192)

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
constexpr const char* KEY_BMODE = "boot_mode";
constexpr const char* KEY_FPWR  = "fox_pwr_idx";
constexpr const char* KEY_MUTE  = "muted";
constexpr const char* KEY_VOL   = "volume";

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

    if (prefs.isKey(KEY_BMODE))
        cached_boot_mode = prefs.getUChar(KEY_BMODE, cached_boot_mode);

    if (prefs.isKey(KEY_FPWR))
        cached_fox_pwr_idx = prefs.getUChar(KEY_FPWR, cached_fox_pwr_idx);

    if (prefs.isKey(KEY_MUTE))
        cached_muted = prefs.getBool(KEY_MUTE, cached_muted);

    if (prefs.isKey(KEY_VOL))
        cached_volume = clamp_u8(prefs.getUChar(KEY_VOL, cached_volume), VOL_MIN, VOL_MAX);

    prefs.end();
}

uint8_t station_id() { return cached_id; }

void set_station_id(uint8_t id) {
    cached_id = id;
    prefs.begin(NS, false);
    prefs.putUChar(KEY_ID, id);
    prefs.end();
}

const char* callsign() { return cached_call; }

void set_callsign(const char* call) {
    if (!call) return;
    strlcpy(cached_call, call, sizeof(cached_call));
    prefs.begin(NS, false);
    prefs.putString(KEY_CALL, cached_call);
    prefs.end();
}

const char* fox_message() { return cached_msg; }

void set_fox_message(const char* msg) {
    if (!msg) return;
    strlcpy(cached_msg, msg, sizeof(cached_msg));
    prefs.begin(NS, false);
    prefs.putString(KEY_MSG, cached_msg);
    prefs.end();
}

uint8_t wpm() { return cached_wpm; }

void set_wpm(uint8_t wpm) {
    cached_wpm = clamp_u8(wpm, WPM_MIN, WPM_MAX);
    if (cached_char_wpm < cached_wpm) cached_char_wpm = cached_wpm;  // keep C >= S
    prefs.begin(NS, false);
    prefs.putUChar(KEY_WPM, cached_wpm);
    prefs.putUChar(KEY_CWPM, cached_char_wpm);
    prefs.end();
}

uint8_t char_wpm() { return cached_char_wpm; }

void set_char_wpm(uint8_t wpm) {
    cached_char_wpm = clamp_u8(wpm, cached_wpm, WPM_MAX);  // never below overall
    prefs.begin(NS, false);
    prefs.putUChar(KEY_CWPM, cached_char_wpm);
    prefs.end();
}

uint8_t boot_mode() { return cached_boot_mode; }

void set_boot_mode(uint8_t mode) {
    if (mode == cached_boot_mode) return;   // avoid a needless flash write
    cached_boot_mode = mode;
    prefs.begin(NS, false);
    prefs.putUChar(KEY_BMODE, mode);
    prefs.end();
}

uint8_t fox_pwr_idx() { return cached_fox_pwr_idx; }

void set_fox_pwr_idx(uint8_t idx) {
    if (idx == cached_fox_pwr_idx) return;   // avoid a needless flash write
    cached_fox_pwr_idx = idx;
    prefs.begin(NS, false);
    prefs.putUChar(KEY_FPWR, idx);
    prefs.end();
}

uint8_t volume() { return cached_volume; }

void set_volume(uint8_t units) {
    uint8_t v = clamp_u8(units, VOL_MIN, VOL_MAX);
    if (v == cached_volume) return;   // avoid a needless flash write
    cached_volume = v;
    prefs.begin(NS, false);
    prefs.putUChar(KEY_VOL, v);
    prefs.end();
}

bool muted() { return cached_muted; }

void set_muted(bool m) {
    if (m == cached_muted) return;   // avoid a needless flash write
    cached_muted = m;
    prefs.begin(NS, false);
    prefs.putBool(KEY_MUTE, m);
    prefs.end();
}

} // namespace config
