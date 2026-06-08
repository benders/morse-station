#include "display.h"
#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) || defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
// Heltec V4 / V3 / RAK4631 / Wio Tracker L1 Pro path: 128x64 mono SSD1306-class OLED via U8g2
// (Heltec's on-board SSD1315 and the RAK1921 SSD1306 are both U8g2
// "ssd1306_128x64_noname" panels). The Cardputer ADV has a 240x135 colour
// ST7789V2 driven by M5.Display; see display_cardputer.cpp.
#include "battery.h"
#include "config.h"
#include "pins.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

namespace {

// Set false if the panel can't be initialized — every draw call then becomes a
// no-op so a missing/stuck OLED can't lock up the whole node. Only the RAK /
// Wio Tracker path clears it (see begin()); the Heltec panels are always
// present, so it stays true there and the guards are free.
bool g_oled_ok = true;

#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
// RAK1921 / Wio Tracker L1 Pro OLED: no reset line (U8X8_PIN_NONE), default
// Wire / HW-I2C — the WisCore variant fixes SDA=13/SCL=14, and the Wio
// Tracker variant fixes SDA=D14/SCL=D15, as the Wire defaults, so no explicit
// pin args are needed (unlike the Heltec constructor, which passes its
// VEXT-gated SDA/SCL/RST trio explicitly).
#if defined(DEVICE_WIO_TRACKER_L1)
// The Wio Tracker L1 Pro's 128x64 panel is an SH1106 controller, NOT an
// SSD1306. The SH1106 has 132 columns of RAM with only the middle 128 visible
// (a 2-column offset), so driving it with the SSD1306 init leaves the image
// shifted and a ~2px band of wrapped content on the RIGHT edge ("slight
// right-side distortion"). The SH1106 constructor applies the column offset and
// renders edge-to-edge. (The vendored Meshtastic variant's USE_SSD1306 flag is
// not authoritative — Meshtastic's OLED library auto-probes the controller at
// runtime; here we pin it explicitly.)
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);   // RAK1921
#endif

// (The nRF52 TWIM endTransmission spin-waits on EVENTS_TXSTARTED with no
// timeout, so a bus held low by a mid-transaction slave hangs forever — hence
// the recovery + present-flag gating below.)

// I2C bus recovery: if a slave (e.g. the OLED left mid-byte by the previous
// firmware across our reset) is holding SDA low, the TWIM can't issue a START.
// Bit-bang up to 9 SCL pulses to clock the slave out, then a STOP. Returns
// true once both lines idle high. Pins are open-drain (release = INPUT_PULLUP,
// using the nRF52 internal pull-ups; never drive a line high).
bool i2c_bus_recover(uint8_t sda, uint8_t scl) {
    pinMode(scl, INPUT_PULLUP);
    pinMode(sda, INPUT_PULLUP);
    delayMicroseconds(10);
    for (int i = 0; i < 9 && digitalRead(sda) == LOW; i++) {
        pinMode(scl, OUTPUT); digitalWrite(scl, LOW);   // SCL low
        delayMicroseconds(5);
        pinMode(scl, INPUT_PULLUP);                     // release SCL high
        delayMicroseconds(5);
    }
    // Generate a STOP condition: SDA low->high while SCL is high.
    pinMode(sda, OUTPUT); digitalWrite(sda, LOW); delayMicroseconds(5);
    pinMode(scl, INPUT_PULLUP);                          delayMicroseconds(5);
    pinMode(sda, INPUT_PULLUP);                          delayMicroseconds(5);
    return digitalRead(sda) == HIGH && digitalRead(scl) == HIGH;
}
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
    U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
#endif

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
#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)
    // Recover a possibly-stuck I2C bus before letting U8g2 (and the no-timeout
    // nRF52 TWIM) touch it; skip the OLED entirely if it can't be freed, so a
    // bad bus degrades to "headless node" instead of a boot hang. (A stuck bus
    // is seen in practice when the previous firmware left the OLED mid-byte
    // across our reset, holding SDA low so the TWIM can never issue a START.)
    bool idle = i2c_bus_recover(PIN_WIRE_SDA, PIN_WIRE_SCL);

    // Probe the panel (0x3C, with 0x3D fallback) so the serial log says whether
    // it actually ACKs — only safe now that the bus is known idle.
    uint8_t addr = 0;
    if (idle) {
        Wire.begin();
        for (uint8_t a = 0x3C; a <= 0x3D; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) { addr = a; break; }
        }
    }
    Serial.printf("# oled: bus=%s addr=%s0x%02X\n",
                  idle ? "idle" : "STUCK", addr ? "" : "none/", addr ? addr : 0x3C);
    g_oled_ok = (idle && addr != 0);
    if (!g_oled_ok) return;             // headless: all draw calls no-op below
    if (addr == 0x3D) oled.setI2CAddress(0x3D << 1);
#else
    // Heltec V4/V3 gate the OLED's 3.3V rail behind VEXT_CTRL. The RAK19007
    // base board (and the Wio Tracker L1 Pro) has no such switched peripheral
    // rail — the OLED is always powered — so this step is skipped entirely
    // for RAK4631 / DEVICE_WIO_TRACKER_L1.
    pinMode(PIN_VEXT_CTRL, OUTPUT);
    digitalWrite(PIN_VEXT_CTRL, LOW);   // power peripheral rail (OLED)
    delay(50);
#endif
    oled.begin();
    oled.setFont(u8g2_font_6x12_tr);
}

void menu(const char* const* items, int n, int sel) {
    if (!g_oled_ok) return;
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
    if (!g_oled_ok) return;
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
    if (!g_oled_ok) return;
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);

    // Header: "RECV <ID>" of the station being copied, tone "*" after the title.
    char hdr[20];
    if (station_id >= 0) snprintf(hdr, sizeof(hdr), "RECV %d", station_id);
    else                 snprintf(hdr, sizeof(hdr), "RECV ---");
    oled.drawStr(0, 11, hdr);
    int bx = draw_battery();
    if (tone_on) oled.drawStr(bx - 4 - 6, 11, "*");

    // "MUTE" left-aligned just after the station header when silenced, so the
    // operator can tell a quiet node from a muted one (see sidetone_set_mute).
    // Fixed position (not anchored to the "*") so it stays put as the tone
    // indicator blinks. Font is 6px-wide (u8g2_font_6x12_tr).
    if (config::muted()) oled.drawStr((int)strlen(hdr) * 6 + 8, 11, "MUTE");

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
    if (!g_oled_ok) return;
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
    if (!g_oled_ok) return;
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    if (title) oled.drawStr(0, 14, title);
    if (line1) oled.drawStr(0, 36, line1);
    if (line2) oled.drawStr(0, 54, line2);
    oled.sendBuffer();
}

} // namespace display
#endif // DEVICE_HELTEC_V4 || DEVICE_HELTEC_V3 || DEVICE_RAK4631 || DEVICE_WIO_TRACKER_L1
