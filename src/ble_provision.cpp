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
NimBLEService*        g_service   = nullptr;   // current NUS service (rebuilt each begin)
NimBLEServer*         g_server    = nullptr;
bool                  g_inited    = false;   // host stack currently inited
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

    // NimBLE-Arduino 2.x: power-cycle the BLE controller on every begin()/stop()
    // so AUTO mode can drop the 2.4 GHz core (~70 mA) when the panel blanks.
    // stop() uses deinit(false) because deinit(true) STILL double-free-panics
    // under 2.x on the ESP32-S3 (heap_caps_free assert — bench-confirmed), so
    // clean teardown is off the table. See the fresh-service rebuild dance below
    // and in stop() for how we keep the notify/console path working across
    // repeated off/on cycles despite deinit(false) leaving the GATT objects
    // behind. Validated: 3+ off/on cycles + multi-toggle, BLE console responsive
    // every time, exactly one NUS service/char in the GATT table afterward.
    g_asm_len = 0;
    if (!g_queue) g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(Line));

    NimBLEDevice::init(adv_name);

    // createServer() returns the SAME singleton across init/deinit(false)
    // cycles, and its m_svcVec (with any prior NUS service) survives deinit.
    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_srv_cb);

    // Build the NUS service + characteristics FRESH on every begin(). The old
    // service object (if any) was marked for delete by stop() while the host was
    // still alive; createService()/svc->start() -> resetGATT() now actually
    // deletes it and registers this fresh one with new handles. Creating a fresh
    // service also flips the server's internal "services changed" flag, which is
    // what forces resetGATT() to re-register the GATT DB against the re-inited
    // host. (Reusing the old service object instead left m_gattsStarted=true, so
    // start() no-op'd and the host exposed an EMPTY GATT table — the central
    // connected but found no NUS characteristics: the "silent console" bug.
    // Naively recreating WITHOUT removing the old one in stop() instead produced
    // a DUPLICATE TX characteristic, also breaking the notify path.)
    NimBLEService* svc = g_server->createService(NUS_SERVICE_UUID);
    g_service = svc;

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

void notify(const char* line) {
    if (!g_tx_char || !g_connected || !line) return;
    BleOut out;
    out.print(line);
    out.print('\n');
}

bool connected() { return g_connected; }

void stop() {
    if (!g_started) return;
    // deinit(false): deinit(true) STILL double-free-panics on the ESP32-S3 under
    // NimBLE 2.x (heap_caps_free assert — bench-confirmed), so clean teardown is
    // off the table. deinit(false) releases the BLE host + controller (powers
    // down the 2.4 GHz core, the ~70 mA drop) WITHOUT walking/freeing the GATT
    // C++ objects.
    g_started   = false;
    g_inited    = false;
    g_connected = false;

    // Mark the current NUS service for deletion WHILE THE HOST IS STILL ALIVE.
    // removeService() needs a live host to set the service's GATT visibility; do
    // it before deinit(). This (a) flips the server's services-changed flag and
    // (b) queues the stale service object for actual deletion by resetGATT() on
    // the next begin()/start(). Without this, the surviving service object would
    // be re-registered alongside a freshly-created one -> DUPLICATE TX
    // characteristic, which kills the notify/console path after rebuild.
    if (g_server && g_service) {
        g_server->removeService(g_service, /*deleteSvc=*/true);
    }
    g_service = nullptr;
    g_tx_char = nullptr;

    NimBLEDevice::deinit(false);
}

}  // namespace ble_provision

#endif // DEVICE_HELTEC_V4 || DEVICE_HELTEC_V3 || DEVICE_CARDPUTER_ADV
