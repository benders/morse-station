#include "radio.h"
#include "pins.h"
#include <RadioLib.h>

// nRF52 has no IRAM and no IRAM_ATTR macro; the ESP32 one only matters for
// placing ISR code in IRAM so it survives flash-cache-disabled sections (e.g.
// during NVS writes). Neutralize it on nRF52 so on_rx() compiles unchanged.
#if (defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)) && !defined(IRAM_ATTR)
#define IRAM_ATTR
#endif

namespace {

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
// No second SPI peripheral the way the ESP32-S3 exposes HSPI/VSPI — both
// nRF52840 boards' SX1262 lives on the global `SPI` (re-pinned explicitly via
// SPI.setPins()/begin() for the RAK; the Wio's variant default SPI pins ARE
// the LoRa pins, so a bare SPI.begin() suffices — see init() below and
// §2 / reference/rak4631 + reference/wio-tracker-l1-pro READMEs).
SX1262 chip = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, SPI);
#else
SPIClass radioSpi(HSPI);
SX1262   chip = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, radioSpi);
#endif

// Link parameters (mirror docs/protocol.md). Single channel for now.
constexpr float    FREQ_MHZ      = 905.0f;
constexpr float    BITRATE_KBPS  = 4.8f;
constexpr float    FREQ_DEV_KHZ  = 5.0f;
// 23.4 kHz: the ~Carson signal width (~15 kHz for 4.8k/5k dev) plus a generous
// frequency-offset/temperature cushion (~8.6 kHz, ~9.5 ppm @ 905 MHz). This was
// 78.2 kHz on the theory that the SX1262 TCXO trims differed by ~12-31 kHz
// (~13-34 ppm) board-to-board — a figure taken from early GFSK-burst bench
// readings, where the apparent "offset" was really demod/measurement error. A
// clean unmodulated-CW measurement over an rtl_tcp SDR (the `txcw` command +
// scripts/sdr_drift.py, docs/frequency-drift.md) put every unit within <1 ppm
// (<~1 kHz) of the others, demolishing the headroom rationale. A round-robin
// link test (scripts/foxhunt_roundrobin.py --rxbw) then confirmed 0-3% loss on
// every pair at 78.2 / 39.0 / 19.5 kHz alike — including the RAK<->Wio link the
// old comment claimed was "100% deaf" at 39 kHz. Narrowing buys ~3 dB/octave of
// sensitivity (78.2->23.4 ~= 5 dB) at no measured link cost on the bench; the
// field weak-signal range gain is the open item. Runtime-overridable + persisted
// via the `rxbw` console command (radio::set_rx_bw / config::rx_bw_khz), so a
// unit can be re-widened in seconds if a real offset ever shows up.
constexpr float    RX_BW_KHZ     = 23.4f;   // boot default; runtime in s_rx_bw_khz
// Live RX filter width, settable via radio::set_rx_bw() (the `rxbw` console
// command) and restored from NVS at boot. Starts at the begin() default so a
// board with no persisted value matches beginFSK().
float              s_rx_bw_khz   = RX_BW_KHZ;
constexpr int      TX_POWER_DBM  = 2;       // low power, 15.249-ish
constexpr int      PREAMBLE_BITS = 16;
// Per-board SX1262 reference (DIO3-driven TCXO supply voltage) and power-supply
// mode (useRegulatorLDO: true = LDO only, false = DC-DC) passed straight into
// beginFSK(). Getting TCXO_V wrong does NOT fail init — the chip still keys a
// carrier off the underpowered/over-driven TCXO, but it drifts. The Cardputer
// taught us that the hard way (see below).
#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)
// Heltec V4 and V3 both use a 1.8 V TCXO on the on-board SX1262.
constexpr float    TCXO_V        = 1.8f;
constexpr bool     USE_LDO       = false;
#elif defined(DEVICE_CARDPUTER_ADV)
// Cap LoRa-1262 (removable module). The TCXO is 3.0 V, NOT 1.8 V — confirmed by
// M5Stack's authoritative Arduino example, which calls radio.begin(..., 3.0,
// true) (docs.m5stack.com/en/cap/Cap_LoRa-1262 -> Arduino Quick Start). With the
// wrong 1.8 V the SDR saw the carrier wander ~50 kHz with kHz jitter and ~35 dB
// SNR (docs/frequency-drift.md); TCXO_V=0 (plain crystal) killed init outright,
// proving a real TCXO is present. 3.0 V + LDO is the vendor-blessed config.
constexpr float    TCXO_V        = 3.0f;
constexpr bool     USE_LDO       = true;
#elif defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
// RAK4631 WisCore module: SX1262's DIO3 drives a 1.8 V TCXO (§2 of the port
// plan / reference/rak4631). The Wio Tracker L1 Pro's Wio-SX1262 module uses
// the same 1.8 V TCXO (confirmed in W1 / reference/wio-tracker-l1-pro).
constexpr float    TCXO_V        = 1.8f;
constexpr bool     USE_LDO       = false;
#endif

// Sync word identifying our net (2 bytes).
uint8_t SYNC_WORD[] = {0x2D, 0xD4};

volatile bool rx_flag = false;

void IRAM_ATTR on_rx() { rx_flag = true; }

} // namespace

#ifdef HAS_FEM
// Which external FEM this board actually carries. Auto-detected at boot (see
// fem_power_on) rather than chosen by a build flag, so one firmware image runs
// on both Heltec V4 revisions:
//   V4.2 -> GC1109   : antenna TX/RX switch is on DIO2; CPS (GPIO46) selects the
//                      PA path (HIGH) vs bypass (LOW). GPIO5 is unused.
//   V4.3 -> KCT8103L : CTX (GPIO5) is the software TX/RX switch AND RX LNA
//                      select; CSD (GPIO2) gates power. GPIO46 is unused.
// Mirrors MeshCore's variants/heltec_v4/LoRaFEMControl.
enum FemType { FEM_GC1109, FEM_KCT8103L };
static FemType s_fem = FEM_KCT8103L;   // overwritten by detect in fem_power_on()

// Runtime RX LNA select (V4.3 KCT8103L only). True = CTX LOW in RX, routing
// through the LNA; false = CTX HIGH in RX, bypassing the LNA (antenna feeds the
// SX1262 without front-end gain). Defaults on (boot is sensitive RX). Only
// affects RX — TX always bypasses the LNA (CTX HIGH). See set_lna()/fem_set_rx().
static bool s_lna_on = true;

// Current FEM PA state. On the GC1109 (V4.3 KCT8103L has no bypass pin) this
// drives CPS during TX: HIGH = full PA (+~6 dB), LOW = bypass. set_pa() updates
// it; fem_set_tx() applies it. Mirrors fem_power_on()'s boot-bypass default.
static bool s_pa_on = false;

// Drive the FEM into its receive configuration for the detected part.
void fem_set_rx() {
    if (s_fem == FEM_KCT8103L) {
        // V4.3: CSD is the chip POWER-DOWN line (HIGH = FEM powered); CTX is the
        // LNA select for the RX path -- LOW routes through the LNA (sensitive),
        // HIGH bypasses it. Keep CSD HIGH and toggle CTX for runtime LNA control.
        // Matches MeshCore LoRaFEMControl::setRxModeEnable.
        digitalWrite(PIN_FEM_CSD, HIGH);
        digitalWrite(PIN_FEM_CTX, s_lna_on ? LOW : HIGH);
    } else {
        // V4.2 GC1109: EN HIGH keeps the chip enabled; CPS LOW selects the RX /
        // bypass path. The antenna TX/RX switch itself is on DIO2 (handled by
        // setDio2AsRfSwitch). GPIO5 (CTX) is unused on this revision — left alone.
        digitalWrite(PIN_FEM_CSD, HIGH);
        digitalWrite(PIN_FEM_CPS, LOW);
    }
}
// Drive the FEM into its transmit configuration for the detected part.
void fem_set_tx() {
    if (s_fem == FEM_KCT8103L) {
        digitalWrite(PIN_FEM_CSD, HIGH);                 // FEM powered for the PA
        digitalWrite(PIN_FEM_CTX, HIGH);                 // LNA bypassed / TX path
    } else {
        digitalWrite(PIN_FEM_CSD, HIGH);                 // GC1109 enabled
        digitalWrite(PIN_FEM_CPS, s_pa_on ? HIGH : LOW); // full PA vs bypass
    }
}

void fem_power_on() {
    // 1. Power the FEM LDO first, so the PA chip's internal CSD pull is actually
    //    energised before we sense it. delay() lets the rail and chip settle.
    pinMode(PIN_FEM_VFEM, OUTPUT);
    digitalWrite(PIN_FEM_VFEM, HIGH);   // power the FEM LDO (both revs)
    delay(1);                           // FEM startup time after power-on

    // 2. Auto-detect the FEM type via the shared CSD pin's (GPIO2) default pull.
    //    GC1109 CSD has an internal pull-DOWN  -> floats LOW  -> V4.2.
    //    KCT8103L CSD has an internal pull-UP   -> floats HIGH -> V4.3.
    //    Same trick as MeshCore LoRaFEMControl::init().
    pinMode(PIN_FEM_CSD, INPUT);
    delay(1);
    s_fem = (digitalRead(PIN_FEM_CSD) == HIGH) ? FEM_KCT8103L : FEM_GC1109;

    // 3. Take the control pins as outputs and land in a known RX/bypass state.
    pinMode(PIN_FEM_CSD, OUTPUT);
    digitalWrite(PIN_FEM_CSD, HIGH);    // chip enable (GC1109 EN / KCT8103L CSD)
    pinMode(PIN_FEM_CPS, OUTPUT);
    digitalWrite(PIN_FEM_CPS, LOW);     // V4.2 PA bypass (low power); unused on V4.3
    pinMode(PIN_FEM_CTX, OUTPUT);       // V4.3 TX/RX + LNA select; unused on V4.2
    s_pa_on = false;                    // never come up hot — bypass on every boot
    fem_set_rx();                       // default to RX/LNA until we transmit
}
#else
inline void fem_set_rx() {}
inline void fem_set_tx() {}
#endif // HAS_FEM

namespace radio {

float frequency_mhz() { return FREQ_MHZ; }

bool init(int& err) {
#ifdef HAS_FEM
    fem_power_on();
#endif

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
#if defined(DEVICE_RAK4631)
    // Power the SX1262's onboard LDO. Without this the radio simply does not
    // respond on SPI (beginFSK() times out / SPI_CMD errors) — easy to miss,
    // called out explicitly in §2 of the port plan. The Wio has no such pin
    // (always-on LDO, confirmed W1) — do NOT drive a power-enable for it.
    pinMode(SX126X_POWER_EN, OUTPUT);
    digitalWrite(SX126X_POWER_EN, HIGH);
    delay(10);                      // let the LDO rail settle before SPI traffic

    SPI.setPins(PIN_MISO, PIN_SCK, PIN_MOSI);
#endif
    SPI.begin();
#else
    radioSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
#endif
    err = chip.beginFSK(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ,
                        TX_POWER_DBM, PREAMBLE_BITS, TCXO_V, USE_LDO);
    if (err != RADIOLIB_ERR_NONE) return false;

    // 2-byte hardware CRC. beginFSK() already defaults this on, but pin it
    // explicitly so a future RadioLib default change can't silently disable it:
    // the decoder relies on corrupted frames being dropped (poll() returns false
    // on RADIOLIB_ERR_CRC_MISMATCH), since a bit-flipped-but-accepted packet
    // would decode to a confident wrong character. The only RX integrity check
    // is this 16-bit CRC — packets carry just a 1-byte MAGIC, no app checksum.
    err = chip.setCRC(2);
    if (err != RADIOLIB_ERR_NONE) return false;

    // DIO2 drives the TX/RX antenna switch. On the Heltec V4 that's the FEM CTX
    // line; the Cap LoRa-1262 wires DIO2 to its on-cap RF switch the same way.
    err = chip.setDio2AsRfSwitch(true);
    if (err != RADIOLIB_ERR_NONE) return false;

#if defined(DEVICE_WIO_TRACKER_L1)
    // This board is NOT pure-DIO2: the vendored variant.h additionally defines
    // a discrete RXEN gate (SX126X_RXEN = D5 / "LoRa_SW") in series with the
    // DIO2-driven switch — DIO2 alone leaves RX deaf. Register it with RadioLib
    // so it toggles RXEN automatically around transmit()/startReceive(), the
    // same way upstream Meshtastic drives SX126x boards that combine
    // SX126X_DIO2_AS_RF_SWITCH with a separate SX126X_RXEN/SX126X_TXEN pair
    // (see meshtastic/firmware src/mesh/SX126xInterface.cpp: it always calls
    // lora.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN), defaulting either pin to
    // RADIOLIB_NC when undefined). The variant defines SX126X_TXEN as
    // RADIOLIB_NC (TX path is handled by DIO2), so we pass it straight through.
    //
    // HARDWARE-UNVALIDATED (no unit yet as of W3) — confirm with an on-air RX
    // check in W9; if RX is still deaf, try driving SX126X_RXEN HIGH manually
    // instead of/in addition to setRfSwitchPins().
    chip.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN);
#endif

    err = chip.setSyncWord(SYNC_WORD, sizeof(SYNC_WORD));
    if (err != RADIOLIB_ERR_NONE) return false;

    chip.setPacketReceivedAction(on_rx);
    return true;
}

bool send(const uint8_t* data, size_t len) {
    fem_set_tx();                  // KCT8103L (V4.3): switch FEM to the TX path
    int state = chip.transmit(const_cast<uint8_t*>(data), len);
    fem_set_rx();                  // back to RX/LNA so we don't sit LNA-bypassed
    return state == RADIOLIB_ERR_NONE;
}

bool set_tx_power(int dbm) {
    return chip.setOutputPower(dbm) == RADIOLIB_ERR_NONE;
}

bool tx_cw(bool on) {
    if (on) {
        fem_set_tx();                         // FEM -> TX config (PA per s_pa_on)
        // transmitDirect() with frf=0 keeps the current channel and issues
        // SetTxContinuousWave: an unmodulated carrier at frequency_mhz(). It also
        // sets the RF switch to TX, matching what transmit() does in send().
        return chip.transmitDirect() == RADIOLIB_ERR_NONE;
    }
    chip.standby();                           // stop the carrier
    start_receive();                          // FEM back to RX + re-arm receive
    return true;
}

int sleep() {
    // SX126x SetSleep (warm-start; config retention is irrelevant — the MCU is
    // about to power off too). Drops the transceiver from standby (~1.5 mA) to
    // ~1.2 µA. fem_off would also help on the V4, but hibernate() handles the
    // FEM/Vext rails; here we only command the SX1262 itself.
    int state = chip.sleep();
#if defined(DEVICE_RAK4631)
    // The RAK4631 gates the SX1262 LDO behind SX126X_POWER_EN — cut it for a
    // truly unpowered radio (no such gate on Heltec/Wio: always-on rail).
    digitalWrite(SX126X_POWER_EN, LOW);
#endif
    return state;
}

bool has_fem() {
#ifdef HAS_FEM
    return true;
#else
    return false;
#endif
}

const char* fem_name() {
#ifdef HAS_FEM
    return s_fem == FEM_KCT8103L ? "KCT8103L (V4.3)" : "GC1109 (V4.2)";
#else
    return "none";
#endif
}

void set_pa(bool on) {
#ifdef HAS_FEM
    // GC1109 (V4.2): CPS selects the PA path (HIGH, +~6 dB) vs bypass (LOW). The
    // V4.3 (KCT8103L) has no bypass pin, so this is a no-op there. We record the
    // state and let fem_set_tx() apply CPS at the right moment (during send the
    // FEM is in TX config); applying it here too keeps a bench A/B immediate.
    s_pa_on = on;
    if (s_fem == FEM_GC1109) digitalWrite(PIN_FEM_CPS, on ? HIGH : LOW);
#else
    (void)on;
#endif
}

void set_lna(bool on) {
#ifdef HAS_FEM
    s_lna_on = on;
    fem_set_rx();   // re-apply CSD now if we're sitting in receive
#else
    (void)on;
#endif
}

bool lna_on() {
#ifdef HAS_FEM
    return s_lna_on;
#else
    return false;
#endif
}

// Supported SX1262 GFSK RX bandwidths (kHz, double-sideband) — the same LUT
// RadioLib's findRxBw() snaps to. We mirror it so set_rx_bw() can report the
// value actually programmed instead of the raw request (RadioLib keeps it
// internal). Nearest-by-absolute-difference, matching findRxBw.
static float snap_rx_bw(float khz) {
    static const float lut[] = {
        4.8f, 5.8f, 7.3f, 9.7f, 11.7f, 14.6f, 19.5f, 23.4f, 29.3f, 39.0f,
        46.9f, 58.6f, 78.2f, 93.8f, 117.3f, 156.2f, 187.2f, 234.3f, 312.0f,
        373.6f, 467.0f};
    float best = lut[0];
    float bd = (khz > best) ? khz - best : best - khz;
    for (float v : lut) {
        float d = (khz > v) ? khz - v : v - khz;
        if (d < bd) { bd = d; best = v; }
    }
    return best;
}

float rx_bw_khz() { return s_rx_bw_khz; }

bool set_rx_bw(float khz) {
    float snapped = snap_rx_bw(khz);
    chip.standby();                       // reprogram modulation params off-air
    int st = chip.setRxBandwidth(snapped);
    if (st == RADIOLIB_ERR_NONE) s_rx_bw_khz = snapped;
    start_receive();                      // re-arm RX (FEM back to RX) at new BW
    return st == RADIOLIB_ERR_NONE;
}

void start_receive() {
    fem_set_rx();                  // KCT8103L (V4.3): LNA in the RX path
    rx_flag = false;
    chip.startReceive();
}

bool poll(uint8_t* out, size_t max_len, size_t& out_len, float& rssi_dbm) {
    if (!rx_flag) return false;
    rx_flag = false;

    size_t avail = chip.getPacketLength();
    size_t n = avail < max_len ? avail : max_len;
    int state = chip.readData(out, n);
    rssi_dbm = chip.getRSSI();
    chip.startReceive();           // re-arm

    // LOAD-BEARING: this also rejects RADIOLIB_ERR_CRC_MISMATCH, so a frame that
    // failed the 16-bit hardware CRC is dropped here before reaching the decoder.
    // readData() fills `out` even on a CRC failure (RadioLib lets you keep corrupt
    // bytes); do NOT relax this to "salvage" that data — it would feed bit-flipped
    // packets to the Morse decoder as confident wrong characters. See begin()'s
    // setCRC(2) note.
    if (state != RADIOLIB_ERR_NONE) return false;
    out_len = n;
    return true;
}

} // namespace radio
