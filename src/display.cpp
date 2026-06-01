#include "display.h"
#ifndef DEVICE_CARDPUTER_ADV
// Heltec V4 path: 128x64 mono SSD1306 OLED via U8g2. The Cardputer ADV has a
// 240x135 colour ST7789V2 driven by M5.Display; see display_cardputer.cpp.
#include "battery.h"
#include "pins.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

namespace {

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);

// Map RSSI to a 0..128 px bar width. -110 dBm (weak) .. -40 dBm (strong).
int rssi_to_px(float dbm) {
    const float lo = -110.0f, hi = -40.0f;
    if (dbm < lo) dbm = lo;
    if (dbm > hi) dbm = hi;
    return (int)((dbm - lo) / (hi - lo) * 128.0f);
}

// Draw a small battery glyph anchored at the top-right of the header row, with
// a fill proportional to charge. Returns the x of its left edge so callers can
// right-justify other header text clear of it. On the 1-bit OLD there's no
// colour cue, so a near-empty pack is shown by an empty (unfilled) body.
int draw_battery() {
    const int bw = 18, bh = 9, ty = 1;   // body w/h, top y
    const int nub = 2;
    const int bx = 128 - bw - nub;       // body left x (nub on the right)
    oled.drawFrame(bx, ty, bw, bh);
    oled.drawBox(bx + bw, ty + 2, nub, bh - 4);
    int pct = battery::percent();
    if (pct > 0) {
        int fw = (bw - 4) * pct / 100;
        if (fw > 0) oled.drawBox(bx + 2, ty + 2, fw, bh - 4);
    }
    return bx;
}

} // namespace

namespace display {

void begin() {
    pinMode(PIN_VEXT_CTRL, OUTPUT);
    digitalWrite(PIN_VEXT_CTRL, LOW);   // power peripheral rail (OLED)
    delay(50);
    oled.begin();
    oled.setFont(u8g2_font_6x12_tr);
}

void menu(const char* const* items, int n, int sel) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 11, "Select mode:");
    oled.drawStr(96, 11, "(PRG)");
    for (int i = 0; i < n; i++) {
        int y = 26 + i * 13;
        if (i == sel) {
            oled.drawBox(0, y - 10, 128, 12);
            oled.setDrawColor(0);
            oled.drawStr(4, y, items[i]);
            oled.setDrawColor(1);
        } else {
            oled.drawStr(4, y, items[i]);
        }
    }
    oled.sendBuffer();
}

void fox(uint16_t seq, const char* msg, bool tone_on, const char* pwr) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 11, tone_on ? "FOX TX  *" : "FOX TX");
    int bx = draw_battery();
    // power level, right-justified on the header row, clear of the battery glyph
    // (PRG cycles it).
    char pbuf[12];
    snprintf(pbuf, sizeof(pbuf), "PWR %s", pwr);
    oled.drawStr(bx - 4 - (int)strlen(pbuf) * 6, 11, pbuf);
    // message wrapped across two lines (21 chars per line at 6px)
    char line[22];
    size_t len = strlen(msg);
    strncpy(line, msg, 21); line[21] = 0;
    oled.drawStr(0, 30, line);
    if (len > 21) { strncpy(line, msg + 21, 21); line[21] = 0; oled.drawStr(0, 44, line); }
    char buf[20];
    snprintf(buf, sizeof(buf), "seq=%u", seq);
    oled.drawStr(0, 62, buf);
    oled.sendBuffer();
}

void hunter(const char* text, float freq_mhz, int station_id, bool ditdah,
            float rssi_dbm, bool rssi_valid, bool tone_on) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);

    // Header: "RECV <ID>" of the station being copied, tone "*" after the title.
    char hdr[20];
    if (station_id >= 0) snprintf(hdr, sizeof(hdr), "RECV %d", station_id);
    else                 snprintf(hdr, sizeof(hdr), "RECV ---");
    oled.drawStr(0, 11, hdr);
    int bx = draw_battery();
    if (tone_on) oled.drawStr(bx - 4 - 6, 11, "*");

    // Operating frequency, right-justified on the header-adjacent row.
    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%.1f MHz", freq_mhz);
    oled.drawStr(128 - (int)strlen(fbuf) * 6, 25, fbuf);

    // Mode label, then a single scrolling line of recent copy. 21 glyphs fit
    // across the 128px panel at 6px; show that many of the most recent letters
    // (or dit/dah elements), scrolling off the left as new copy arrives.
    const size_t SHOW = 21;
    size_t len = strlen(text);
    const char* tail = len > SHOW ? text + (len - SHOW) : text;
    oled.drawStr(0, 25, ditdah ? "./-" : "TXT");
    oled.drawStr(0, 47, tail);

    // RSSI bar
    oled.drawFrame(0, 54, 128, 10);
    if (rssi_valid) oled.drawBox(0, 54, rssi_to_px(rssi_dbm), 10);
    oled.sendBuffer();
}

void livekey(uint16_t seq, bool tone_on) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 11, tone_on ? "LIVE KEY  *" : "LIVE KEY");
    draw_battery();
    oled.drawStr(0, 30, "keying on air");
    char buf[20];
    snprintf(buf, sizeof(buf), "seq=%u", seq);
    oled.drawStr(0, 62, buf);
    oled.sendBuffer();
}

void status(const char* title, const char* line1, const char* line2) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    if (title) oled.drawStr(0, 14, title);
    if (line1) oled.drawStr(0, 36, line1);
    if (line2) oled.drawStr(0, 54, line2);
    oled.sendBuffer();
}

} // namespace display
#endif // !DEVICE_CARDPUTER_ADV
