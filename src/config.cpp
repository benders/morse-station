#include "config.h"
#include <Arduino.h>
#include <Preferences.h>

namespace {
Preferences prefs;
uint8_t cached_id = 1;
constexpr const char* NS  = "morse";
constexpr const char* KEY = "station_id";
}

namespace config {

void begin() {
    prefs.begin(NS, false);
    if (prefs.isKey(KEY)) {
        cached_id = prefs.getUChar(KEY, 1);
    } else {
        // First boot: pick a random id in 1..254 (255 is broadcast).
        cached_id = (uint8_t)(esp_random() % 254) + 1;
        prefs.putUChar(KEY, cached_id);
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
