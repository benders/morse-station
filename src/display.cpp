#include "display.h"
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
    // power level, right-justified on the header row (PRG cycles it)
    char pbuf[12];
    snprintf(pbuf, sizeof(pbuf), "PWR %s", pwr);
    oled.drawStr(128 - (int)strlen(pbuf) * 6, 11, pbuf);
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

void hunter(const char* text, float freq_mhz, const char* call, bool ditdah,
            float rssi_dbm, bool rssi_valid, bool tone_on) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);

    // Header: "HUNTER <CALL>" left, tone "*" indicator tucked after the title.
    char hdr[20];
    snprintf(hdr, sizeof(hdr), "HUNTER %s", (call && call[0]) ? call : "----");
    oled.drawStr(0, 11, hdr);
    if (tone_on) oled.drawStr(122, 11, "*");

    // Operating frequency, right-justified on the header-adjacent row.
    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%.1f MHz", freq_mhz);
    oled.drawStr(128 - (int)strlen(fbuf) * 6, 25, fbuf);

    // Mode label, then a single scrolling line of recent copy. 21 glyphs fit at
    // 6px; show the last 16 (decoded letters) or the last ~16 dit/dah elements,
    // scrolling off the left as new copy arrives.
    const size_t SHOW = 16;
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
