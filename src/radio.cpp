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
// 78.2 kHz, not the ~Carson minimum (~15 kHz for 4.8k/5k dev). The extra width
// is deliberate frequency-offset headroom: these SX1262 modules' TCXO trims
// differ enough board-to-board that a relative carrier offset of ~12-31 kHz was
// measured between the RAK4631 and Wio nRF52 units (~13-34 ppm @ 905 MHz). At
// the old 39 kHz that offset fell OUTSIDE the RX filter and the RAK->Wio link
// was 100% deaf at every power level; 78.2 kHz catches it (39 dead / 78 & 156
// work — bracketed on the bench). Cost is ~3 dB sensitivity vs 39 kHz. Mesh
// firmwares sidestep this entirely by running wide LoRa (Meshtastic LongFast =
// 250 kHz), which also tolerates ~25% of BW in offset; narrow FSK does not.
constexpr float    RX_BW_KHZ     = 78.2f;
constexpr int      TX_POWER_DBM  = 2;       // low power, 15.249-ish
constexpr int      PREAMBLE_BITS = 16;
#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)
// Heltec V4 and V3 both use a 1.8 V TCXO on the on-board SX1262.
constexpr float    TCXO_V        = 1.8f;
#elif defined(DEVICE_CARDPUTER_ADV)
// VERIFY ON HW: Cap LoRa-1262. If beginFSK() fails (RADIOLIB_ERR_SPI_CMD_*),
// the module likely uses a plain crystal — set this to 0.0f.
constexpr float    TCXO_V        = 1.8f;
#elif defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
// RAK4631 WisCore module: SX1262's DIO3 drives a 1.8 V TCXO (§2 of the port
// plan / reference/rak4631). The Wio Tracker L1 Pro's Wio-SX1262 module uses
// the same 1.8 V TCXO (confirmed in W1 / reference/wio-tracker-l1-pro).
constexpr float    TCXO_V        = 1.8f;
#endif

// Sync word identifying our net (2 bytes).
uint8_t SYNC_WORD[] = {0x2D, 0xD4};

volatile bool rx_flag = false;

void IRAM_ATTR on_rx() { rx_flag = true; }

} // namespace

#ifdef HAS_FEM
// Power and enable the external front-end module (PA + LNA). The V4 needs this
// or RX is badly desensitised. We drive the superset of both board revisions'
// control pins (see pins.h): the pins a given revision doesn't use are just
// free GPIOs there, so driving them is harmless.
//   CSD = 1 -> chip enabled (GC1109 EN / KCT8103L CSD)
//   CPS = 0 -> GC1109 PA bypass, matching our low-power (~+2 dBm) operation.
// CTX (GPIO5) is left to fem_set_rx()/fem_set_tx(): on the V4.3 KCT8103L it is
// the software TX/RX switch *and* the RX LNA select, so it must track the radio
// state. (On the V4.2 GC1109 the real CTX is DIO2; GPIO5 is a free pin there.)
void fem_set_rx() { digitalWrite(PIN_FEM_CTX, LOW); }   // KCT8103L: LNA in RX path
void fem_set_tx() { digitalWrite(PIN_FEM_CTX, HIGH); }  // KCT8103L: TX path

void fem_power_on() {
    pinMode(PIN_FEM_VFEM, OUTPUT);
    digitalWrite(PIN_FEM_VFEM, HIGH);   // power the FEM LDO (both revs)
    pinMode(PIN_FEM_CSD, OUTPUT);
    digitalWrite(PIN_FEM_CSD, HIGH);    // chip enable (GC1109 EN / KCT8103L CSD)
    pinMode(PIN_FEM_CPS, OUTPUT);
    digitalWrite(PIN_FEM_CPS, LOW);     // V4.2 PA bypass (low power)
    pinMode(PIN_FEM_CTX, OUTPUT);
    fem_set_rx();                       // default to RX/LNA until we transmit
    delay(1);                           // let the FEM power up
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
                        TX_POWER_DBM, PREAMBLE_BITS, TCXO_V, false);
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

bool has_fem() {
#ifdef HAS_FEM
    return true;
#else
    return false;
#endif
}

void set_pa(bool on) {
#ifdef HAS_FEM
    // CPS HIGH -> GC1109 (V4.2) PA path engaged (+~6 dB); LOW -> bypass. On the
    // V4.3 (KCT8103L) this pin is freed and the part has no bypass, so this is a
    // no-op there. CTX (TX/RX) still tracks via DIO2 / fem_set_tx() during send.
    digitalWrite(PIN_FEM_CPS, on ? HIGH : LOW);
#else
    (void)on;
#endif
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

    if (state != RADIOLIB_ERR_NONE) return false;
    out_len = n;
    return true;
}

} // namespace radio
