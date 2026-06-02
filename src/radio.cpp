#include "radio.h"
#include "pins.h"
#include <RadioLib.h>

namespace {

SPIClass radioSpi(HSPI);
SX1262   chip = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, radioSpi);

// Link parameters (mirror docs/protocol.md). Single channel for now.
constexpr float    FREQ_MHZ      = 905.0f;
constexpr float    BITRATE_KBPS  = 4.8f;
constexpr float    FREQ_DEV_KHZ  = 5.0f;
constexpr float    RX_BW_KHZ     = 39.0f;   // ~Carson BW for 4.8k/5k dev, w/ margin
constexpr int      TX_POWER_DBM  = 2;       // low power, 15.249-ish
constexpr int      PREAMBLE_BITS = 16;
#if defined(DEVICE_HELTEC_V4)
constexpr float    TCXO_V        = 1.8f;    // Heltec V4 SX1262 has a 1.8 V TCXO
#elif defined(DEVICE_CARDPUTER_ADV)
// VERIFY ON HW: Cap LoRa-1262. If beginFSK() fails (RADIOLIB_ERR_SPI_CMD_*),
// the module likely uses a plain crystal — set this to 0.0f.
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

    radioSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
    err = chip.beginFSK(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ,
                        TX_POWER_DBM, PREAMBLE_BITS, TCXO_V, false);
    if (err != RADIOLIB_ERR_NONE) return false;

    // DIO2 drives the TX/RX antenna switch. On the Heltec V4 that's the FEM CTX
    // line; the Cap LoRa-1262 wires DIO2 to its on-cap RF switch the same way.
    err = chip.setDio2AsRfSwitch(true);
    if (err != RADIOLIB_ERR_NONE) return false;

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
