#ifdef DEVICE_CARDPUTER_ADV

#include "config.h"
#include "config_ui.h"
#include "kbd_ui.h"
#include "keyboard.h"
#include "protocol.h"
#include <M5Unified.h>
#include <stdlib.h>

// Runtime, keyboard-driven Instructor menu for the Cardputer ADV. Distinct from
// the boot config editor (config_ui_cardputer.cpp): this opens at runtime when an
// idle instructor sees a keypress, lets the operator pick one of the field
// actions, and composes the matching relay/alert command into the caller's
// buffer. The caller (main.cpp) feeds it to handle_setup_command() and lets
// loop() resume so the existing async burst/ACK path runs normally — no radio
// logic lives here.

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

// Labels mirror PWR_LEVELS in main.cpp (LO/MED/HI/MAX); the menu only needs
// the labels and indices, not the dBm values (those live with the radio code).
constexpr const char* kPwrLabels[] = {"LO", "MED", "HI", "MAX"};
constexpr int kNPwr = 4;

void draw_pwr_picker() {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print("FOX POWER");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    for (int i = 0; i < kNPwr; ++i) {
        d.setCursor(0, 26 + i * 18);
        d.printf("%d %s", i + 1, kPwrLabels[i]);
    }
    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, H - 10);
    d.print("1-4 select   ENTER cancel");
}

// 1 Fox power: pick LO/MED/HI/MAX, compose "relay <foxid> pwr <n>".
bool action_fox_power(char* out, size_t cap) {
    if (config::fox_id() == 0) {
        edit_fox_id();
        if (config::fox_id() == 0) return false;   // operator declined; abort
    }
    draw_pwr_picker();
    char c = kbd_ui::wait_key(30000);
    if (c < '1' || c > '0' + kNPwr) return false;   // ENTER/timeout/other -> cancel
    int n = c - '1';
    snprintf(out, cap, "relay %u pwr %d", config::fox_id(), n);
    return true;
}

// 2 Fox message: edit_field seeded from config::fox_message(), persist on
// ENTER, compose "relay <foxid> msg <text>".
bool action_fox_message(char* out, size_t cap) {
    if (config::fox_id() == 0) {
        edit_fox_id();
        if (config::fox_id() == 0) return false;   // operator declined; abort
    }
    char buf[config::FOX_MSG_MAX + 1];
    strncpy(buf, config::fox_message(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    kbd_ui::edit_field("Fox message", buf, sizeof(buf));
    config::set_fox_message(buf);   // persist so it pre-fills next time
    snprintf(out, cap, "relay %u msg %s", config::fox_id(), buf);
    return true;
}

void draw_mute_picker() {
    auto& d = M5.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print("MUTE HUNTERS");
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 26); d.print("1 Mute");
    d.setCursor(0, 44); d.print("2 Unmute");
    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, H - 10);
    d.print("1-2 select   ENTER cancel");
}

// 3 Mute hunters: broadcast toggle, no fox id needed.
bool action_mute(char* out, size_t cap) {
    draw_mute_picker();
    char c = kbd_ui::wait_key(30000);
    if (c == '1') { snprintf(out, cap, "relay 255 mute on"); return true; }
    if (c == '2') { snprintf(out, cap, "relay 255 mute off"); return true; }
    return false;   // ENTER/timeout/other -> cancel
}

// Last RETURN-alert text sent this session. RAM-only by design (plan: typed at
// send time, no NVS preset) — seeds the editor so a repeat send doesn't start
// blank, but never survives a reboot.
char g_last_alert[proto::BCAST_TEXT_MAX + 1] = "";

// 4 RETURN alert: edit_field seeded from the session-local last alert,
// compose "alert <text>". Empty text cancels.
bool action_alert(char* out, size_t cap) {
    char buf[proto::BCAST_TEXT_MAX + 1];
    strncpy(buf, g_last_alert, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    kbd_ui::edit_field("RETURN alert", buf, sizeof(buf));
    if (buf[0] == '\0') return false;   // empty -> cancel, nothing sent
    strncpy(g_last_alert, buf, sizeof(g_last_alert) - 1);
    g_last_alert[sizeof(g_last_alert) - 1] = '\0';
    snprintf(out, cap, "alert %s", buf);
    return true;
}

} // namespace

namespace config_ui {

bool instructor_menu(char* out, size_t cap) {
    // First-run forcing: a fresh instructor has no fox id staged, so prompt
    // for one immediately rather than leaving the ready screen showing
    // "set fox id" until the operator happens to dig into Settings.
    if (config::fox_id() == 0) edit_fox_id();

    for (;;) {
        draw_menu();
        char c = kbd_ui::wait_key(30000);
        if (c == 0 || c == '\r') return false;   // timeout / ENTER -> back to ready draw
        switch (c) {
            case '1':
                if (action_fox_power(out, cap)) return true;
                break;
            case '2':
                if (action_fox_message(out, cap)) return true;
                break;
            case '3':
                if (action_mute(out, cap)) return true;
                break;
            case '4':
                if (action_alert(out, cap)) return true;
                break;
            case '5':
                edit_fox_id();
                break;
            default:
                break;   // ignore; keep the menu up
        }
    }
}

} // namespace config_ui

#endif // DEVICE_CARDPUTER_ADV
