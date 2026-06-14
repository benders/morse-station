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

// Runtime FSK receive-bandwidth (SX1262 DSB filter, kHz). set_rx_bw() snaps the
// request to the nearest supported step (4.8..467 kHz, RadioLib findRxBw),
// reprograms the modem, and re-arms receive; rx_bw_khz() returns the value that
// is actually programmed (post-snap). Applies to every board (not FEM-specific).
// Persistence lives in config (config::set_rx_bw_khz); the `rxbw` console command
// writes it and boot restores via radio::set_rx_bw(config::rx_bw_khz()). Narrower
// = ~3 dB/octave more sensitivity but less frequency-offset headroom; the default
// 78.2 kHz is wide because early bench measurements suggested large TCXO offsets,
// since refuted by the CW/SDR test (<1 ppm). Returns true on success.
bool  set_rx_bw(float khz);
float rx_bw_khz();

// Test aid for SDR frequency-drift measurement: emit an unmodulated continuous
// wave (CW) carrier at the operating frequency (radio::frequency_mhz()), or stop
// it. on=true drives the FEM into TX config and issues the SX1262
// SetTxContinuousWave command (via RadioLib transmitDirect()); on=false returns
// to standby + continuous receive. The carrier sits at exactly the channel
// centre with no FSK modulation, so an SDR can read this transmitter's true
// frequency as a single spectral peak. TX power / FEM PA follow the current
// set_tx_power()/set_pa() state. Returns true on success. While CW is on the
// radio cannot receive. Mind FCC §15.249 (the carrier is a pure unmodulated
// emission) — keep bursts short.
bool tx_cw(bool on);

// Put the radio into continuous receive. Call once after init() on RX nodes.
void start_receive();

// Put the SX1262 into its lowest-power sleep (SetSleep) — used on the hibernate
// path so the radio isn't left in standby (~1.5 mA) drawing current while the
// rest of the node is powered down. On the RAK4631 the SX1262 LDO is also gated
// off (SX126X_POWER_EN LOW) for a fully unpowered radio; the Heltec/Wio have an
// always-on radio rail, so SetSleep (~1.2 µA) is the floor there. The radio must
// have been init()'d first (the command travels over SPI). Returns the RadioLib
// status (0 = RADIOLIB_ERR_NONE) so the caller can log it for verification.
int sleep();

// Non-blocking: if a packet arrived since the last call, copy up to max_len
// bytes into out, set out_len and rssi_dbm, and return true. Re-arms receive.
bool poll(uint8_t* out, size_t max_len, size_t& out_len, float& rssi_dbm);

} // namespace radio
