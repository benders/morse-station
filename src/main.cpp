#include <Arduino.h>
#include "pins.h"
#include "sidetone.h"
#include "morse.h"
#include "radio.h"
#include "protocol.h"

// Stage 5 keystate broadcast over FSK (two units).
//
// Role at boot: hold PRG/BOOT = TX (fox), else RX (hunter).
//   TX: fox Player produces key state; every 30 ms a KeyState packet is sent.
//       The fox also keys its own local sidetone.
//   RX: received key state drives the sidetone and feeds the Morse decoder;
//       decoded text is printed to serial.
//
// Stage 6 generalizes the TX source to a live key and adds OLED + mode select.

static constexpr uint32_t TONE_HZ = 600;
static constexpr uint8_t  WPM     = 13;
static constexpr uint8_t  STATION_ID = 1;
static constexpr uint32_t REPEAT_PAUSE = 3000;
static const char* FOX_MSG = "FOX NEAR THE BIG OAK BY THE LAKE";

static bool is_tx = false;

// TX state
static morse::Player player;
static uint16_t seq = 0;
static uint32_t last_tx = 0;
static uint32_t pause_until = 0;

// RX state
static morse::Decoder decoder;
static bool rx_down = false;

static void set_tone(bool on) {
    static bool cur = false;
    if (on != cur) { cur = on; if (on) sidetone_on(); else sidetone_off(); }
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    pinMode(PIN_MODE_BTN, INPUT_PULLUP);
    is_tx = (digitalRead(PIN_MODE_BTN) == LOW);
    Serial.printf("\n# keystate broadcast — role: %s\n", is_tx ? "TX fox" : "RX hunter");

    sidetone_init(PIN_SIDETONE, TONE_HZ);

    int err;
    if (!radio::init(err)) {
        Serial.printf("FATAL: radio init failed (code=%d)\n", err);
        while (true) delay(1000);
    }

    if (is_tx) {
        player.begin(WPM);
        player.start(FOX_MSG);
    } else {
        decoder.begin(WPM);
        radio::start_receive();
    }
}

static void loop_tx(uint32_t now) {
    // Advance the fox message / handle repeat pause.
    if (!player.finished()) {
        player.update(now);
        if (player.finished()) pause_until = now + REPEAT_PAUSE;
    } else if (now >= pause_until) {
        player.start(FOX_MSG);
    }

    bool down = player.down();
    set_tone(down);     // fox hears itself locally

    if (now - last_tx >= proto::TX_INTERVAL_MS) {
        last_tx = now;
        proto::KeyState ks{proto::MAGIC, STATION_ID, (uint8_t)down, seq++};
        uint8_t buf[proto::PACKET_LEN];
        proto::encode(ks, buf);
        radio::send(buf, proto::PACKET_LEN);
    }
}

static void loop_rx(uint32_t now) {
    uint8_t buf[32];
    size_t n;
    float rssi;
    if (radio::poll(buf, sizeof(buf), n, rssi)) {
        proto::KeyState ks;
        if (proto::decode(buf, n, ks)) {
            rx_down = ks.key_down != 0;
        }
    }
    set_tone(rx_down);

    char c = decoder.update(rx_down, now);
    if (c) Serial.print(c);
}

void loop() {
    uint32_t now = millis();
    if (is_tx) loop_tx(now); else loop_rx(now);
}
