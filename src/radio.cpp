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

namespace radio {

bool init(int& err) {
    radioSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
    err = chip.beginFSK(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ,
                        TX_POWER_DBM, PREAMBLE_BITS, TCXO_V, false);
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
