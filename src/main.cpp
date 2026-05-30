#include <Arduino.h>
#include "pins.h"
#include "radio.h"

// Stage 3 radio link test (needs two Heltec V4 units).
//
// Role is chosen at boot: hold the PRG/BOOT button (PIN_MODE_BTN) while
// resetting to come up as TX (beacon); otherwise the unit is RX (listener).
//   TX: sends an incrementing 4-byte payload once per second.
//   RX: prints any received payload + RSSI to serial.

static bool is_tx = false;
static uint32_t seq = 0;

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    pinMode(PIN_MODE_BTN, INPUT_PULLUP);
    is_tx = (digitalRead(PIN_MODE_BTN) == LOW);

    Serial.println();
    Serial.printf("# radio link test — role: %s\n", is_tx ? "TX (beacon)" : "RX (listen)");

    int err;
    if (!radio::init(err)) {
        Serial.printf("FATAL: radio init failed (code=%d)\n", err);
        while (true) delay(1000);
    }

    if (!is_tx) radio::start_receive();
}

void loop() {
    if (is_tx) {
        uint8_t pkt[4] = {0xA5, (uint8_t)(seq >> 8), (uint8_t)seq, 0x5A};
        bool ok = radio::send(pkt, sizeof(pkt));
        Serial.printf("TX seq=%lu %s\n", (unsigned long)seq, ok ? "ok" : "ERR");
        seq++;
        delay(1000);
    } else {
        uint8_t buf[32];
        size_t n;
        float rssi;
        if (radio::poll(buf, sizeof(buf), n, rssi)) {
            Serial.printf("RX %u bytes rssi=%.1f dBm:", (unsigned)n, rssi);
            for (size_t i = 0; i < n; i++) Serial.printf(" %02X", buf[i]);
            Serial.println();
        }
    }
}
