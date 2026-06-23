#ifdef DEVICE_CARDPUTER_ADV

#include "config.h"
#include "config_ui.h"
#include "kbd_ui.h"
#include "keyboard.h"
#include <M5Unified.h>
#include <stdlib.h>

// Runtime, keyboard-driven Instructor menu for the Cardputer ADV. Distinct from
// the boot config editor (config_ui_cardputer.cpp): this opens at runtime when an
// idle instructor sees a keypress, lets the operator pick one of the field
// actions, and (in later phases) composes the matching relay/alert command and
// feeds it to handle_setup_command() — the same path the BLE/serial console
// uses. No radio logic lives here.
//
// Phase 1 is the menu skeleton only: it renders the five actions and returns to
// the caller's idle draw on ENTER or timeout. None of the actions are wired yet
// (added one per commit in Phase 3). Shares the input layer (kbd_ui) with the
// boot editor.

namespace {

constexpr int H = kbd_ui::H;

void draw_menu() {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print("INSTRUCTOR");

    // Items at text size 2 for legibility. At ~12 px/glyph the 240 px width fits
    // ~20 chars, so labels are kept short (full detail is on each action screen).
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 26); d.print("1 Fox power");
    d.setCursor(0, 44); d.print("2 Fox message");
    d.setCursor(0, 62); d.print("3 Mute hunters");
    d.setCursor(0, 80); d.print("4 RETURN alert");
    d.setCursor(0, 98); d.print("5 Settings");

    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, H - 10);
    d.print("1-5 select   ENTER back");
}

// Edit config::fox_id() via the shared line editor. Accepts 1..254; anything
// else (blank, 0, >=255, non-numeric) is ignored and the stored id is left
// unchanged. ipwr is out of scope for Phase 2 — fox id only.
void edit_fox_id() {
    char buf[4] = "";
    uint8_t cur = config::fox_id();
    if (cur != 0) snprintf(buf, sizeof(buf), "%u", cur);
    kbd_ui::edit_field("Settings -> Fox ID", buf, sizeof(buf));
    int v = atoi(buf);
    if (v >= 1 && v <= 254) config::set_fox_id((uint8_t)v);
}

} // namespace

namespace config_ui {

void instructor_menu() {
    // First-run forcing: a fresh instructor has no fox id staged, so prompt
    // for one immediately rather than leaving the ready screen showing
    // "set fox id" until the operator happens to dig into Settings.
    if (config::fox_id() == 0) edit_fox_id();

    for (;;) {
        draw_menu();
        char c = kbd_ui::wait_key(30000);
        if (c == 0 || c == '\r') return;   // timeout / ENTER -> back to ready draw
        switch (c) {
            case '5':
                edit_fox_id();
                break;
            // Phase 3 wires each of these to a relay/alert command. For now the
            // selections are recognised but do nothing; the menu stays up.
            case '1':   // -> relay <foxid> pwr <n>
            case '2':   // -> relay <foxid> msg <text>
            case '3':   // -> relay 255 mute on|off
            default:
                break;   // ignore; keep the menu up
        }
    }
}

} // namespace config_ui

#endif // DEVICE_CARDPUTER_ADV
