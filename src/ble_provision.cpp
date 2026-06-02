#include "ble_provision.h"

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
bool                  g_started   = false;
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
    void onWrite(NimBLECharacteristic* chr) override {
        std::string val = chr->getValue();
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
    void onConnect(NimBLEServer*) override { g_connected = true; }
    void onDisconnect(NimBLEServer*) override {
        g_connected = false;
        NimBLEDevice::startAdvertising();
    }
};

RxCallbacks     g_rx_cb;
ServerCallbacks g_srv_cb;

}  // namespace

void begin(const char* adv_name, Handler handler) {
    if (g_started) return;
    g_handler = handler;
    g_asm_len = 0;
    if (!g_queue) g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(Line));

    NimBLEDevice::init(adv_name);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&g_srv_cb);
    NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

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
    NimBLEDevice::deinit(true);
    g_tx_char   = nullptr;
    g_handler   = nullptr;
    g_connected = false;
    g_started   = false;
}

}  // namespace ble_provision
