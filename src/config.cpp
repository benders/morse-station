#include "config.h"
#include <Arduino.h>
#include <Preferences.h>

namespace {
Preferences prefs;
uint8_t cached_id = 1;
constexpr const char* NS  = "morse";
constexpr const char* KEY = "station_id";
}

namespace {
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
    if (prefs.isKey(KEY)) {
        cached_id = prefs.getUChar(KEY, 1);   // explicit override, if set
    } else {
        cached_id = mac_station_id();         // stable default from MAC
    }
    prefs.end();
}

uint8_t station_id() { return cached_id; }

void set_station_id(uint8_t id) {
    cached_id = id;
    prefs.begin(NS, false);
    prefs.putUChar(KEY, id);
    prefs.end();
}

} // namespace config
