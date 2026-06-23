#ifdef DEVICE_CARDPUTER_ADV

#include "kbd_ui.h"
#include "keyboard.h"
#include <M5Unified.h>
#include <string.h>

// Shared keyboard + LCD input primitives for the Cardputer ADV. Extracted from
// config_ui_cardputer.cpp so the boot editor and the runtime instructor menu
// share one input layer. Behavior is byte-for-byte the same as the original
// anonymous-namespace helpers.

namespace kbd_ui {

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

} // namespace kbd_ui

#endif // DEVICE_CARDPUTER_ADV
