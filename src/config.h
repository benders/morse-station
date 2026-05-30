#pragma once
#include <stdint.h>

// Per-unit persistent settings in NVS (ESP32 Preferences). Currently just the
// station id: assigned randomly on first boot so two units differ without
// per-device provisioning. Override with config::set_station_id() if you want
// deliberate ids per camp.

namespace config {

void    begin();
uint8_t station_id();
void    set_station_id(uint8_t id);

} // namespace config
