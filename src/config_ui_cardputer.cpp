#ifdef DEVICE_CARDPUTER_ADV

#include "config_ui.h"
#include "kbd_ui.h"
#include "keyboard.h"
#include "config.h"
#include <M5Unified.h>
#include <string.h>

// Keyboard-driven config editor for the Cardputer ADV. Renders directly with
// M5.Display (the editor is its own little screen, distinct from the fox/hunter
// views in display_cardputer.cpp) and reads keys via the keyboard:: driver. The
// input primitives (wait_key/draw_field/edit_field) live in kbd_ui, shared with
// the runtime instructor menu.

namespace {

using kbd_ui::wait_key;
using kbd_ui::edit_field;
constexpr int H = kbd_ui::H;

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
