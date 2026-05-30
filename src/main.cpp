#include <Arduino.h>
#include "pins.h"
#include "sidetone.h"
#include "morsekey.h"
#include "morse.h"
#include "radio.h"
#include "protocol.h"
#include "display.h"

// Stage 6 — integrated fox-hunt firmware.
//
// Boot menu (PRG/BOOT button): short press cycles the highlighted mode, a long
// press (>=600 ms) selects it; auto-selects after 8 s. Modes:
//   HUNTER  — receive keystate, play Morse sidetone, show decoded text + RSSI.
//   FOX     — loop the canned location message, transmit keystate every 30 ms.
//   LIVEKEY — transmit a live telegraph key, play local sidetone.
//
// One protocol (proto::KeyState) carries both FOX and LIVEKEY to any HUNTER.

static constexpr uint32_t TONE_HZ      = 600;
static constexpr uint8_t  WPM          = 13;
static constexpr uint8_t  STATION_ID   = 1;
static constexpr uint32_t REPEAT_PAUSE = 3000;
static const char* FOX_MSG = "FOX NEAR THE BIG OAK BY THE LAKE";

enum Mode { MODE_HUNTER = 0, MODE_FOX = 1, MODE_LIVEKEY = 2 };
static Mode mode = MODE_HUNTER;

static morse::Player  player;
static morse::Decoder decoder;
static MorseKey       key;

static uint16_t seq = 0;
static uint32_t last_tx = 0;
static uint32_t pause_until = 0;
static uint32_t last_draw = 0;

// Rolling decoded-text buffer for the hunter view.
static char text_buf[128];
static size_t text_len = 0;

static void set_tone(bool on) {
    static bool cur = false;
    if (on != cur) { cur = on; if (on) sidetone_on(); else sidetone_off(); }
}

static void text_push(char c) {
    if (text_len + 1 >= sizeof(text_buf)) {
        // drop oldest half to make room
        memmove(text_buf, text_buf + sizeof(text_buf) / 2, sizeof(text_buf) / 2);
        text_len = sizeof(text_buf) / 2;
    }
    text_buf[text_len++] = c;
    text_buf[text_len] = 0;
}

// Blocking boot menu; returns the chosen mode.
static Mode run_menu() {
    static const char* items[] = {"Hunter (RX)", "Fox (TX loop)", "Live key (TX)"};
    const int n = 3;
    int sel = 0;

    pinMode(PIN_MODE_BTN, INPUT_PULLUP);
    display::menu(items, n, sel);

    uint32_t t_start = millis();
    bool pressed = false;
    uint32_t press_t = 0;

    while (true) {
        uint32_t now = millis();
        bool down = (digitalRead(PIN_MODE_BTN) == LOW);

        if (down && !pressed) { pressed = true; press_t = now; }
        else if (!down && pressed) {
            pressed = false;
            uint32_t dur = now - press_t;
            if (dur >= 600) return (Mode)sel;          // long press selects
            sel = (sel + 1) % n;                        // short press cycles
            display::menu(items, n, sel);
            t_start = now;                              // reset idle timeout
        }
        if (now - t_start > 8000) return (Mode)sel;     // idle auto-select
        delay(10);
    }
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    display::begin();
    sidetone_init(PIN_SIDETONE, TONE_HZ);

    mode = run_menu();
    Serial.printf("\n# mode=%d\n", (int)mode);

    int err;
    if (!radio::init(err)) {
        Serial.printf("FATAL: radio init failed (code=%d)\n", err);
        while (true) delay(1000);
    }

    switch (mode) {
        case MODE_FOX:
            player.begin(WPM);
            player.start(FOX_MSG);
            break;
        case MODE_LIVEKEY:
            key.begin(PIN_KEY);
            break;
        case MODE_HUNTER:
            decoder.begin(WPM);
            radio::start_receive();
            break;
    }
}

// Send the current key state as a packet on the 30 ms cadence.
static void tx_keystate(uint32_t now, bool down) {
    if (now - last_tx < proto::TX_INTERVAL_MS) return;
    last_tx = now;
    proto::KeyState ks{proto::MAGIC, STATION_ID, (uint8_t)down, seq++};
    uint8_t buf[proto::PACKET_LEN];
    proto::encode(ks, buf);
    radio::send(buf, proto::PACKET_LEN);
}

static void loop_fox(uint32_t now) {
    if (!player.finished()) {
        player.update(now);
        if (player.finished()) pause_until = now + REPEAT_PAUSE;
    } else if (now >= pause_until) {
        player.start(FOX_MSG);
    }
    bool down = player.down();
    set_tone(down);
    tx_keystate(now, down);

    if (now - last_draw >= 100) { last_draw = now; display::fox(seq, FOX_MSG, down); }
}

static void loop_livekey(uint32_t now) {
    key.update();
    bool down = key.down();
    set_tone(down);
    tx_keystate(now, down);

    if (now - last_draw >= 100) { last_draw = now; display::livekey(seq, down); }
}

static void loop_hunter(uint32_t now) {
    static bool  rx_down = false;
    static float rssi = 0.0f;
    static bool  rssi_valid = false;

    uint8_t buf[32];
    size_t n;
    float r;
    if (radio::poll(buf, sizeof(buf), n, r)) {
        proto::KeyState ks;
        if (proto::decode(buf, n, ks)) {
            rx_down = ks.key_down != 0;
            rssi = r;
            rssi_valid = true;
        }
    }
    set_tone(rx_down);

    char c = decoder.update(rx_down, now);
    if (c) { text_push(c); Serial.print(c); }

    if (now - last_draw >= 100) {
        last_draw = now;
        display::hunter(text_buf, rssi, rssi_valid, rx_down);
    }
}

void loop() {
    uint32_t now = millis();
    switch (mode) {
        case MODE_FOX:     loop_fox(now);     break;
        case MODE_LIVEKEY: loop_livekey(now); break;
        case MODE_HUNTER:  loop_hunter(now);  break;
    }
}
