#pragma once
#include <stdint.h>
#include <stddef.h>

// Thin wrapper over RadioLib's SX1262 in GFSK mode for the Morse station.
// Single fixed channel, low power (FCC 15.249 non-hopping for now; FHSS is a
// later phase). A shared sync word identifies our network.

namespace radio {

// Returns true on success. On failure, err holds the RadioLib status code.
bool init(int& err);

// The fixed operating frequency, in MHz (single-channel for now).
float frequency_mhz();

// Blocking transmit of a small payload. Returns true on success.
bool send(const uint8_t* data, size_t len);

// Set the SX1262 *chip* output power in dBm. On the Heltec V4 the FEM sits after
// this, but its PA mode is not changed here (CPS is fixed at init — see
// fem_power_on); on the Cardputer there is no FEM. Returns true on success. Used
// by the fox to switch high/medium/low for the space at hand.
bool set_tx_power(int dbm);

// True if this board has an external FEM (PA/LNA) — i.e. the Heltec V4.
bool has_fem();

// Human-readable name of the FEM auto-detected at init() (e.g. "KCT8103L (V4.3)"
// or "GC1109 (V4.2)"). The revision is sensed at fem_power_on() by reading the
// shared CSD pin's default pull level — no build flag selects it. Returns "none"
// on boards without a FEM.
const char* fem_name();

// TESTING AID: engage (true) or bypass (false) the V4 FEM power amplifier via
// its CPS line. PA bypass ≈ chip power (+22 dBm ceiling); PA engaged ≈ +28 dBm
// on the V4.2 (GC1109, +~6 dB). Lets a bench A/B +22 vs +28 with no reflash.
// No-op on boards without a FEM, and on the V4.3 (KCT8103L) whose part has no
// CPS bypass pin. NOT persisted — fem_power_on() forces bypass on every boot,
// so a unit never comes up hot. Weigh FCC §15.249 before transmitting engaged.
void set_pa(bool on);

// Runtime RX LNA select for the Heltec V4.3 (KCT8103L) external FEM. on=true
// routes RX through the LNA (CTX LOW — sensitive, the boot default); on=false
// bypasses the LNA (CTX HIGH — antenna straight to the SX1262, for A/B-ing
// front-end gain vs. a strong nearby fox, or chasing LNA overload). The FEM
// stays powered either way (CSD HIGH); CTX is the LNA select, matching MeshCore.
// Affects RX only — TX always bypasses the LNA. No-op on the V4.2 (GC1109), whose
// GPIO5 is unused, and on non-FEM boards. Takes effect immediately. This call
// only drives the pin; persistence lives in config (config::set_lna), which the
// `lna` command writes and boot restores via radio::set_lna(config::lna()).
void set_lna(bool on);
bool lna_on();

// Put the radio into continuous receive. Call once after init() on RX nodes.
void start_receive();

// Non-blocking: if a packet arrived since the last call, copy up to max_len
// bytes into out, set out_len and rssi_dbm, and return true. Re-arms receive.
bool poll(uint8_t* out, size_t max_len, size_t& out_len, float& rssi_dbm);

} // namespace radio
