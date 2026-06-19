// ble_provision_nrf52.cpp — nRF52840 implementation of the ble_provision::
// API, using the Adafruit Bluefruit nRF52 BLE stack's BLEUart service (which
// already speaks the Nordic UART Service / NUS protocol that the ESP32 NimBLE
// path stands up by hand — see ble_provision.cpp). Same line-assembler /
// Print-sink / handle_setup_command dispatch shape, polled from process() on
// the main loop (no FreeRTOS queue needed: Bluefruit's BLEUart buffers RX
// internally and we drain it synchronously, same single-threaded model the
// ESP32 path uses its queue to achieve).
//
// Guarded to compile only for the RAK4631 target. The NimBLE/NUS sibling
// (ble_provision.cpp) is guarded to ESP32-only — see its header comment.

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)

#include "ble_provision.h"
#include <Arduino.h>
#include <bluefruit.h>

namespace ble_provision {

namespace {

Handler       g_handler   = nullptr;
bool          g_started   = false;
volatile bool g_connected = false;

BLEUart bleuart;

// Max line length, matching the ESP32 path / serial console.
constexpr size_t RX_LINE_MAX = 160;

// Line-assembly buffer — process() runs on the main loop only, so this is
// single-threaded (Bluefruit's RX callback just marks data available; we read
// it from here, not from the callback).
char   g_asm[RX_LINE_MAX];
size_t g_asm_len = 0;

// Conservative notify chunk size — BLEUart negotiates the connection MTU
// itself and chunks internally, but we cap writes to a safe default-MTU-sized
// piece anyway so behaviour matches the ESP32 BleOut (20 bytes = 23-byte ATT
// MTU minus the 3-byte ATT header, the un-negotiated default).
constexpr size_t TX_CHUNK = 20;

// Print sink that forwards everything handle_setup_command() writes back to
// the central via BLEUart, chunked the same way the ESP32 BleOut does.
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
        if (!g_connected) return;
        size_t off = 0;
        while (off < size) {
            size_t n = size - off;
            if (n > TX_CHUNK) n = TX_CHUNK;
            bleuart.write(buf + off, n);
            off += n;
        }
    }
};

void connect_cb(uint16_t /*conn_handle*/) {
    g_connected = true;
}

void disconnect_cb(uint16_t /*conn_handle*/, uint8_t /*reason*/) {
    g_connected = false;
    g_asm_len = 0;
    // Bluefruit auto-restarts advertising on disconnect when
    // Bluefruit.Advertising.restartOnDisconnect(true) is set (default), so the
    // node stays reachable for the whole exercise — matching the ESP32 path's
    // explicit NimBLEDevice::startAdvertising() in onDisconnect.
}

} // namespace

void begin(const char* adv_name, Handler handler) {
    if (g_started) return;
    g_handler = handler;
    g_asm_len = 0;

    Bluefruit.begin();
#if defined(DEVICE_WIO_TRACKER_L1)
    // On the Wio Tracker L1 Pro, the vendored variant aliases LED_BLUE
    // (= PIN_LED2 = D12 = P1.00) to the BUZZER pin. Bluefruit's auto
    // connection LED blinks LED_BLUE while advertising, which would toggle the
    // piezo at the blink rate -> a constant "tick". Disable it so the buzzer is
    // only driven by the sidetone path. (The RAK4631, where LED_BLUE is a real
    // LED, keeps the default.)
    Bluefruit.autoConnLed(false);
#endif
    Bluefruit.setName(adv_name);
    Bluefruit.Periph.setConnectCallback(connect_cb);
    Bluefruit.Periph.setDisconnectCallback(disconnect_cb);

    bleuart.begin();

    // Advertise the NUS service UUID (BLEUart registers it) so generic
    // BLE-UART terminal apps (nRF Connect, LightBlue, Bluefruit Connect) find
    // it the same way they find the ESP32 NimBLE NUS service.
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(bleuart);
    Bluefruit.ScanResponse.addName();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244);   // units of 0.625ms (20ms..152.5ms)
    Bluefruit.Advertising.setFastTimeout(30);     // seconds in fast-advertise mode
    Bluefruit.Advertising.start(0);               // 0 = advertise indefinitely

    g_started = true;
}

// Drain whatever BLEUart has buffered, assemble it into lines (same
// terminator/length rules as the ESP32 RxCallbacks::onWrite), and dispatch
// each complete line through the handler with a BleOut sink. Runs entirely on
// the main-loop task — no cross-task queue required.
void process() {
    if (!g_started || !g_handler) return;
    while (bleuart.available()) {
        int ci = bleuart.read();
        if (ci < 0) break;
        char c = (char)ci;
        if (c == '\r' || c == '\n') {
            if (g_asm_len == 0) continue;     // skip blank/CRLF pairs
            g_asm[g_asm_len] = 0;
            BleOut out;
            g_handler(g_asm, out);
            g_asm_len = 0;
        } else if (g_asm_len + 1 < RX_LINE_MAX) {
            g_asm[g_asm_len++] = c;
        }
        // else: line too long — drop the byte, wait for a terminator.
    }
}

void notify(const char* line) {
    if (!g_started || !g_connected || !line) return;
    BleOut out;
    out.print(line);
    out.print('\n');
}

bool connected() { return g_connected; }

void stop() {
    if (!g_started) return;
    // Stop advertising and disconnect any central; Bluefruit doesn't expose a
    // full deinit the way NimBLEDevice does (the SoftDevice stays resident),
    // which is fine — we only call stop() right before system_off(), and a
    // full power-off doesn't need the radio core released first the way the
    // ESP32 does (no risk of the NimBLE deinit double-free this avoided).
    Bluefruit.Advertising.stop();
    if (Bluefruit.connected()) Bluefruit.disconnect(Bluefruit.connHandle());
    g_handler   = nullptr;
    g_connected = false;
    g_started   = false;
}

}  // namespace ble_provision

#endif // DEVICE_RAK4631 || DEVICE_WIO_TRACKER_L1
