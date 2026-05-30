#include "radio.h"
#include "pins.h"
#include <RadioLib.h>

namespace {

SPIClass radioSpi(HSPI);
SX1262   chip = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, radioSpi);

// Link parameters (mirror design-notes.md). Single channel for now.
constexpr float    FREQ_MHZ      = 905.0f;
constexpr float    BITRATE_KBPS  = 4.8f;
constexpr float    FREQ_DEV_KHZ  = 5.0f;
constexpr float    RX_BW_KHZ     = 39.0f;   // ~Carson BW for 4.8k/5k dev, w/ margin
constexpr int      TX_POWER_DBM  = 2;       // low power, 15.249-ish
constexpr int      PREAMBLE_BITS = 16;
constexpr float    TCXO_V        = 1.8f;

// Sync word identifying our net (2 bytes).
uint8_t SYNC_WORD[] = {0x2D, 0xD4};

volatile bool rx_flag = false;

void IRAM_ATTR on_rx() { rx_flag = true; }

} // namespace

// Power and enable the external front-end module (PA + LNA). The V4 needs this
// or RX is badly desensitised. We drive the superset of both board revisions'
// control pins (see pins.h): the pins a given revision doesn't use are just
// free GPIOs there, so driving them is harmless.
//   RX-LNA path: CSD/ctrl = 1, CTX = 0  (CTX is DIO2, auto)
//   CPS = 0 -> PA bypass, matching our low-power (~+2 dBm) operation.
void fem_enable() {
    pinMode(PIN_FEM_VFEM, OUTPUT);
    digitalWrite(PIN_FEM_VFEM, HIGH);   // power the FEM LDO (both revs)
    pinMode(PIN_FEM_CSD, OUTPUT);
    digitalWrite(PIN_FEM_CSD, HIGH);    // V4.2 GC1109 chip enable
    pinMode(PIN_FEM_CTRL, OUTPUT);
    digitalWrite(PIN_FEM_CTRL, HIGH);   // V4.3.1 KCT8103L: LNA in RX path
    // VERIFY ON HW (V4.3.1): this control line is held statically HIGH. Confirm
    // against the KCT8103L datasheet that a constant HIGH selects the RX/LNA
    // path and does NOT latch the FEM into a fixed mode that fights the DIO2/CTX
    // TX/RX switch. If the part needs the mode pin to track TX vs RX, this must
    // follow DIO2 instead of sitting constant. Power-up is fine either way;
    // it's the switching behaviour to scope (TX power out + RX sensitivity).
    pinMode(PIN_FEM_CPS, OUTPUT);
    digitalWrite(PIN_FEM_CPS, LOW);     // V4.2 PA bypass (low power)
    delay(1);                           // let the FEM power up
}

namespace radio {

bool init(int& err) {
    fem_enable();

    radioSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
    err = chip.beginFSK(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ,
                        TX_POWER_DBM, PREAMBLE_BITS, TCXO_V, false);
    if (err != RADIOLIB_ERR_NONE) return false;

    // Let DIO2 drive the FEM TX/RX switch (CTX): high in TX, low otherwise.
    err = chip.setDio2AsRfSwitch(true);
    if (err != RADIOLIB_ERR_NONE) return false;

    err = chip.setSyncWord(SYNC_WORD, sizeof(SYNC_WORD));
    if (err != RADIOLIB_ERR_NONE) return false;

    chip.setPacketReceivedAction(on_rx);
    return true;
}

bool send(const uint8_t* data, size_t len) {
    int state = chip.transmit(const_cast<uint8_t*>(data), len);
    return state == RADIOLIB_ERR_NONE;
}

bool set_tx_power(int dbm) {
    return chip.setOutputPower(dbm) == RADIOLIB_ERR_NONE;
}

void start_receive() {
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
