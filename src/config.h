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

// Overall (effective) keying speed, clamped to 5..40 wpm. Default 15.
uint8_t wpm();
void    set_wpm(uint8_t wpm);

// Farnsworth character speed, clamped to wpm()..40. Default 18. When equal to
// wpm() the keying reduces to plain timing.
uint8_t char_wpm();
void    set_char_wpm(uint8_t wpm);

// Keying transmit mode: 0 = compat (legacy KeyState stream), 1 = edge
// (EdgeEvent on key edges — see docs/edge-events.md). Persisted so a fox flipped
// to edge over the air comes back up in edge mode. Restored into g_keymode at
// boot (main.cpp); the `keymode` console command sets it live and writes through
// here. Default 0 (compat) so a fresh/legacy unit behaves exactly as before.
uint8_t keymode();
void    set_keymode(uint8_t mode);

// Last-selected boot mode (the Mode enum in main.cpp, stored as a raw uint8_t).
// The boot menu starts highlighted on this and persists the chosen mode, so a
// unit powers back up in whatever it was last used as. Default 0 (Hunter).
uint8_t boot_mode();
void    set_boot_mode(uint8_t mode);

// Last-selected fox TX power level, stored as a raw index into PWR_LEVELS in
// main.cpp. The PRG tap cycles it and persists; on boot the fox restores it so
// a power cycle keeps the last-used level. Default 0 (LO). main.cpp clamps to
// its valid range in case the table size changes.
uint8_t fox_pwr_idx();
void    set_fox_pwr_idx(uint8_t idx);

// Sidetone volume, in units of GAIN_Q15/1024 (so 8 -> gain 8192, 32 -> full
// swing 32768). Clamped to 1..32. Persisted and applied at boot via
// sidetone_set_level(); the serial/BLE `vol` command sets it live. Default 8.
uint8_t volume();
void    set_volume(uint8_t units);

// Sidetone mute. Persisted so a node provisioned silent comes back silent after
// a power cycle. Applied at boot (main.cpp) via sidetone_set_mute() and toggled
// live by the Cardputer 'm' key / the BLE `mute` command. Default false.
bool muted();
void set_muted(bool muted);

// V4.3 (KCT8103L) FEM RX LNA select. Persisted so a unit deliberately set to
// bypass (e.g. it sits close to a strong fox and the LNA overloads) stays that
// way across power cycles. Applied at boot (main.cpp) via radio::set_lna() and
// toggled live by the `lna` console command. Default true (LNA in path). No
// effect on non-FEM boards or the V4.2 (its GPIO5 is unused).
bool lna();
void set_lna(bool on);

// Compile-time platform name (heltec-v4 / wio-tracker-l1 / ...), ALWAYS correct
// for the firmware variant. The Heltec V4 sub-revision (V4.2 GC1109 vs V4.3
// KCT8103L) is NOT pinned here — it is auto-detected from the FEM strap at boot
// (radio::fem_name()) and `show` derives the displayed model from that, so the
// model can never be wrong relative to the unit. There is deliberately no manual
// model override.
const char* default_board_model();

} // namespace config
