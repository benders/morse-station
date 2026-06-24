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

// Persist any pending settings changes to flash. Setters above only update the
// RAM cache and mark the value dirty; the actual flash write is deferred here
// so it never runs synchronously inside a radio RX/TX call (field note §2: an
// nRF52 LittleFS write racing the SoftDevice mid-radio-operation rebooted the
// node). Call once per main loop() iteration, outside any RX/TX critical
// section; a no-op when nothing is dirty.
void flush();

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

// Fox canned-message delivery mode (field note §7, docs/plan-text-message-mode.md):
// 0 = keyed (key the clue as edge Morse over the air) / 1 = text (send the clue as
// a single MAGIC_TEXT frame + retransmit burst; hunter renders Morse locally).
// Persisted so a fox flipped over the air comes back up the same; restored into
// g_msgmode at boot (main.cpp). Applies to the canned loop_fox cycle only —
// MODE_LIVEKEY always keys edges. Default 1 (text): it is robust at the edge of
// range; `msgmode keyed` is available for testing the keyed path.
uint8_t msgmode();
void    set_msgmode(uint8_t mode);

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

// FSK receive-bandwidth filter (SX1262 DSB), in kHz. Persisted so a unit
// provisioned to a narrower filter keeps it across power cycles. Applied at boot
// (main.cpp) via radio::set_rx_bw() and changed live by the `rxbw` console
// command, which writes back the snapped value. Default 23.4 kHz (narrowed from
// 78.2 — see radio.cpp RX_BW_KHZ). Stored as deci-kHz (the kv store has no float).
float rx_bw_khz();
void  set_rx_bw_khz(float khz);

// Instructor remote-control sequence counter (the seq stamped on control packets,
// uint8 wrapping mod 256). Persisted so an instructor reboot does NOT restart at
// 0 and collide with a target fox's still-remembered last_seq (which would make
// the next command look like a duplicate and be silently dropped). Loaded into
// g_ctrl_seq at boot (main.cpp) and written back each time a command is staged.
uint8_t ctrl_seq();
void    set_ctrl_seq(uint8_t seq);

// Session fox station id, persisted so the Instructor menu (Cardputer ADV)
// doesn't have to be retyped per relay/alert command. Sentinel 0 = unset
// (the instructor's own id); valid fox ids are 1..254 (255 is broadcast).
// Set via the Instructor menu's Settings item; instructor_menu() forces this
// to be set on first run before the menu is usable.
uint8_t fox_id();
void    set_fox_id(uint8_t id);

// Compile-time platform name (heltec-v4 / wio-tracker-l1 / ...), ALWAYS correct
// for the firmware variant. The Heltec V4 sub-revision (V4.2 GC1109 vs V4.3
// KCT8103L) is NOT pinned here — it is auto-detected from the FEM strap at boot
// (radio::fem_name()) and `show` derives the displayed model from that, so the
// model can never be wrong relative to the unit. There is deliberately no manual
// model override.
const char* default_board_model();

} // namespace config
