#include "ble_provision.h"
#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) || defined(DEVICE_CARDPUTER_ADV)
// NimBLE/NUS transport — ESP32-only. The nRF52 (RAK4631) port supplies the
// same ble_provision:: API via Adafruit Bluefruit BLEUart in
// ble_provision_nrf52.cpp (Phase 2; see RAK-port-plan.md §P2.7).

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Nordic UART Service (NUS) UUIDs — the de-facto standard a generic BLE-UART
// terminal app speaks.
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // phone -> device (write)
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // device -> phone (notify)

namespace ble_provision {

namespace {

Handler               g_handler   = nullptr;
NimBLECharacteristic* g_tx_char   = nullptr;
NimBLEServer*         g_server    = nullptr;
bool                  g_inited    = false;   // one-time NimBLE stack + GATT setup
bool                  g_started   = false;   // advertising active
volatile bool         g_connected = false;

// Max line length, matching the generous serial line length in main.cpp.
constexpr size_t RX_LINE_MAX = 160;

// Completed RX lines wait here until process() drains them on the main task, so
// the parser (which mutates NVS config) never runs on the NimBLE host task.
struct Line { char buf[RX_LINE_MAX]; };
QueueHandle_t g_queue = nullptr;
constexpr size_t QUEUE_DEPTH = 4;

// Line-assembly buffer, owned by the NimBLE host task (onWrite is serialized).
char   g_asm[RX_LINE_MAX];
size_t g_asm_len = 0;

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
        if (!g_tx_char || !g_connected) return;
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
    // NimBLE-Arduino 2.x: onWrite gains a required NimBLEConnInfo& (peer info).
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& /*connInfo*/) override {
        std::string val = chr->getValue();  // NimBLEAttValue -> std::string
        for (char c : val) {
            if (c == '\r' || c == '\n') {
                if (g_asm_len == 0) continue;        // skip blank/CRLF pairs
                g_asm[g_asm_len] = 0;
                if (g_queue) {
                    Line line;
                    memcpy(line.buf, g_asm, g_asm_len + 1);
                    // Drop the line rather than block the host task if the main
                    // loop is behind; provisioning is interactive and low-rate.
                    xQueueSend(g_queue, &line, 0);
                }
                g_asm_len = 0;
            } else if (g_asm_len + 1 < RX_LINE_MAX) {
                g_asm[g_asm_len++] = c;
            }
            // else: line too long — drop the byte, wait for a terminator.
        }
    }
};

// Re-advertise on disconnect so the node stays reachable for the whole exercise
// (NimBLE stops advertising once a central connects).
class ServerCallbacks : public NimBLEServerCallbacks {
    // NimBLE-Arduino 2.x: onConnect/onDisconnect gain a required NimBLEConnInfo&,
    // and onDisconnect adds an int reason code.
    void onConnect(NimBLEServer*, NimBLEConnInfo& /*connInfo*/) override {
        g_connected = true;
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& /*connInfo*/,
                      int /*reason*/) override {
        g_connected = false;
        // Re-advertise only while we're meant to be up — not after stop()
        // intentionally pulled advertising (the BLE-off / panel-blank case).
        if (g_started) NimBLEDevice::startAdvertising();
    }
};

RxCallbacks     g_rx_cb;
ServerCallbacks g_srv_cb;

}  // namespace

void begin(const char* adv_name, Handler handler) {
    if (g_started) return;
    g_handler = handler;

    // SPIKE (NimBLE-Arduino 2.x): full stack + GATT teardown/rebuild on every
    // begin()/stop() cycle. In 1.4.x this leaked the GATT singletons and the
    // re-created NUS characteristic came back disconnected from the notify path
    // (so we kept the stack alive and toggled only advertising). 2.x reworked
    // object ownership so deinit(true) should free everything and a fresh
    // createServer/createService/createCharacteristic should rebuild a working
    // notify path — letting AUTO mode actually drop the BT controller (~70 mA)
    // each time the panel blanks, not just stop advertising. This is what we're
    // here to validate on the ESP32-S3.
    g_asm_len = 0;
    if (!g_queue) g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(Line));

    NimBLEDevice::init(adv_name);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_srv_cb);
    NimBLEService* svc = g_server->createService(NUS_SERVICE_UUID);

    g_tx_char = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* rx_char = svc->createCharacteristic(
        NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx_char->setCallbacks(&g_rx_cb);

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->enableScanResponse(true);   // 2.x rename of setScanResponse(bool)

    NimBLEDevice::startAdvertising();
    g_inited  = true;
    g_started = true;
}

void process() {
    if (!g_queue || !g_handler) return;
    Line line;
    while (xQueueReceive(g_queue, &line, 0) == pdTRUE) {
        BleOut out;
        g_handler(line.buf, out);
    }
}

bool connected() { return g_connected; }

void stop() {
    if (!g_started) return;
    // SPIKE: full teardown. deinit(true) releases the BLE host + controller AND
    // walks/frees the server/service/characteristic objects — powering down the
    // 2.4 GHz core (the ~70 mA idle draw on the S3) rather than merely halting
    // advertising. In 1.4.x this double-free-panicked; the spike is to confirm
    // 2.x frees cleanly so begin() can rebuild on the next panel wake.
    g_started = false;
    g_inited  = false;
    NimBLEDevice::deinit(true);
    g_server    = nullptr;
    g_tx_char   = nullptr;
    g_connected = false;
}

}  // namespace ble_provision

#endif // DEVICE_HELTEC_V4 || DEVICE_HELTEC_V3 || DEVICE_CARDPUTER_ADV
