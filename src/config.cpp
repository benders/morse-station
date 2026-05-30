#include "config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {
Preferences prefs;
uint8_t cached_id = 1;
char    cached_call[config::CALLSIGN_MAX + 1] = "N0CALL";
char    cached_msg[config::FOX_MSG_MAX + 1]   = "DE N0CALL FOX NEAR THE BIG OAK BY THE LAKE";

constexpr const char* NS       = "morse";
constexpr const char* KEY_ID   = "station_id";
constexpr const char* KEY_CALL = "callsign";
constexpr const char* KEY_MSG  = "fox_msg";

// Default station id derived from the factory eFuse MAC, folded into 1..254
// (255 is broadcast). Stable per unit with no provisioning or stored value.
// See espressif/arduino-esp32#932.
uint8_t mac_station_id() {
    uint64_t mac = ESP.getEfuseMac();
    uint8_t x = 0;
    for (int i = 0; i < 6; i++) x ^= (uint8_t)(mac >> (8 * i));
    return (uint8_t)(x % 254) + 1;
}
}

namespace config {

void begin() {
    prefs.begin(NS, false);

    cached_id = prefs.isKey(KEY_ID) ? prefs.getUChar(KEY_ID, 1)
                                    : mac_station_id();

    if (prefs.isKey(KEY_CALL))
        prefs.getString(KEY_CALL, cached_call, sizeof(cached_call));
    if (prefs.isKey(KEY_MSG))
        prefs.getString(KEY_MSG, cached_msg, sizeof(cached_msg));

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

} // namespace config
