#include "ble_provision.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

// Nordic UART Service (NUS) UUIDs — the de-facto standard a generic BLE-UART
// terminal app speaks.
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // phone -> device (write)
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // device -> phone (notify)

namespace ble_provision {

namespace {

Handler              g_handler  = nullptr;
NimBLECharacteristic* g_tx_char = nullptr;
bool                 g_started  = false;

// Line buffer for assembling RX bytes until a CR/LF terminator. Matches the
// generous serial line length in main.cpp's run_setup_console.
char   g_line[160];
size_t g_line_len = 0;

// Conservative notify chunk size. The default ATT MTU is 23 (so 20 payload
// bytes); chunking to 20 is safe before any MTU negotiation.
constexpr size_t TX_CHUNK = 20;

// Print sink that forwards everything handle_setup_command() writes back to the
// central as TX-characteristic notifications, chunked to a safe MTU size.
class BleOut : public Print {
public:
    size_t write(uint8_t b) override {
        uint8_t c = b;
        emit(&c, 1);
        return 1;
    }
    size_t write(const uint8_t* buf, size_t size) override {
        emit(buf, size);
        return size;
    }

private:
    void emit(const uint8_t* buf, size_t size) {
        if (!g_tx_char) return;
        size_t off = 0;
        while (off < size) {
            size_t n = size - off;
            if (n > TX_CHUNK) n = TX_CHUNK;
            g_tx_char->setValue(buf + off, n);
            g_tx_char->notify();
            off += n;
        }
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr) override {
        std::string val = chr->getValue();
        for (char c : val) {
            if (c == '\r' || c == '\n') {
                if (g_line_len == 0) continue;     // skip blank/CRLF pairs
                g_line[g_line_len] = 0;
                BleOut out;
                if (g_handler) g_handler(g_line, out);
                g_line_len = 0;
            } else if (g_line_len + 1 < sizeof(g_line)) {
                g_line[g_line_len++] = c;
            }
            // else: line too long — drop the byte, wait for a terminator.
        }
    }
};

RxCallbacks g_rx_cb;

}  // namespace

void begin(const char* adv_name, Handler handler) {
    if (g_started) return;
    g_handler  = handler;
    g_line_len = 0;

    NimBLEDevice::init(adv_name);

    NimBLEServer* server = NimBLEDevice::createServer();
    NimBLEService* svc   = server->createService(NUS_SERVICE_UUID);

    g_tx_char = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* rx_char = svc->createCharacteristic(
        NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx_char->setCallbacks(&g_rx_cb);

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();

    g_started = true;
}

void stop() {
    if (!g_started) return;
    NimBLEDevice::deinit(true);
    g_tx_char  = nullptr;
    g_handler  = nullptr;
    g_started  = false;
}

}  // namespace ble_provision
