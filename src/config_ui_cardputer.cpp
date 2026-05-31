#ifdef DEVICE_CARDPUTER_ADV

#include "config_ui.h"
#include "keyboard.h"
#include "config.h"
#include <M5Unified.h>
#include <string.h>

// Keyboard-driven config editor for the Cardputer ADV. Renders directly with
// M5.Display (the editor is its own little screen, distinct from the fox/hunter
// views in display_cardputer.cpp) and reads keys via the keyboard:: driver.

namespace {

constexpr int W = 240;
constexpr int H = 135;

// Wait up to timeout_ms for any keypress. Returns the char, or 0 on timeout.
char wait_key(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    char c;
    while (millis() - t0 < timeout_ms) {
        if (keyboard::read_char(c)) return c ? c : ' ';
        delay(10);
    }
    return 0;
}

void draw_field(const char* title, const char* buf) {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print(title);

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 40);
    d.print(buf);
    d.print('_');                      // cursor

    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, H - 12);
    d.print("ENTER save  DEL bksp");
}

// Edit `buf` (NUL-terminated, capacity cap incl. NUL) in place. Blocks until
// ENTER. Backspace deletes; printable chars (and tab->space) append up to cap-1.
void edit_field(const char* title, char* buf, size_t cap) {
    size_t len = strlen(buf);
    draw_field(title, buf);
    for (;;) {
        char c;
        if (!keyboard::read_char(c)) { delay(8); continue; }
        if (c == '\r') return;                          // commit
        if (c == '\b') { if (len) buf[--len] = 0; draw_field(title, buf); continue; }
        if (c == '\t') c = ' ';
        if (c < 0x20 || c > 0x7E) continue;             // ignore other controls
        if (len + 1 < cap) { buf[len++] = c; buf[len] = 0; draw_field(title, buf); }
    }
}

void draw_menu() {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print("Keyboard setup");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 34); d.print("1  Callsign");
    d.setCursor(0, 58); d.print("2  Message");
    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, H - 30);
    d.printf("call: %s", config::callsign());
    d.setCursor(0, H - 18);
    d.print("ENTER  done");
}

} // namespace

namespace config_ui {

void run() {
    keyboard::begin();

    // Brief opt-in window, mirroring the 2 s serial console prompt.
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 8);  d.print("Keyboard setup?");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 50); d.print("press any key");
    d.setTextSize(1);
    d.setCursor(0, H - 14); d.print("(2s, else skip)");

    if (wait_key(2000) == 0) return;                    // no key -> normal boot

    for (;;) {
        draw_menu();
        char c = wait_key(60000);
        if (c == 0 || c == '\r') return;                // done / idle
        if (c == '1') {
            char buf[config::CALLSIGN_MAX + 1];
            strlcpy(buf, config::callsign(), sizeof(buf));
            edit_field("Callsign", buf, sizeof(buf));
            config::set_callsign(buf);
        } else if (c == '2') {
            char buf[config::FOX_MSG_MAX + 1];
            strlcpy(buf, config::fox_message(), sizeof(buf));
            edit_field("Message", buf, sizeof(buf));
            config::set_fox_message(buf);
        }
    }
}

} // namespace config_ui

#endif // DEVICE_CARDPUTER_ADV
