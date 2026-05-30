#pragma once
#include <stdint.h>
#include <stddef.h>

// Per-unit persistent settings in NVS (ESP32 Preferences): station id, operator
// callsign, and the canned fox message. station_id defaults to an eFuse-MAC
// value (stable per unit, no provisioning); callsign and fox_message are the
// operator's identity/content and have only placeholder defaults until set via
// the boot serial console (see config::set_* and the setup REPL in main.cpp).

namespace config {

// Buffer sizes (callsign matches proto::CALLSIGN_MAX; message is generous).
constexpr size_t CALLSIGN_MAX = 10;
constexpr size_t FOX_MSG_MAX  = 96;

void begin();

uint8_t station_id();
void    set_station_id(uint8_t id);

const char* callsign();              // e.g. "N0CALL" until provisioned
void        set_callsign(const char* call);

const char* fox_message();           // keyed CW loop; include your call in it
void        set_fox_message(const char* msg);

} // namespace config
