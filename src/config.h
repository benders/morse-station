#pragma once
#include <stdint.h>

// Per-unit persistent settings in NVS (ESP32 Preferences). Currently just the
// station id. The default is derived from the eFuse MAC (stable per unit, no
// provisioning); an explicit config::set_station_id() is stored in NVS and
// takes precedence for deliberate per-camp ids.

namespace config {

void    begin();
uint8_t station_id();
void    set_station_id(uint8_t id);

} // namespace config
