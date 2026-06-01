#ifdef DEVICE_CARDPUTER_ADV

#include "display.h"
#include "battery.h"
#include "platform_cardputer.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>

// Cardputer ADV display: 240x135 colour ST7789V2 via M5GFX.
//
// Reimplements the same display:: API the Heltec OLED path provides, laid out
// for the larger colour panel, in a green-on-black "console" look. M5.begin()
// (cardputer_m5_begin) owns the LCD.
//
// All drawing goes into an off-screen M5Canvas (full-frame back buffer) that is
// pushed in one blit per frame. Drawing straight to the panel and clearing it
// each frame flickered visibly in the ~10 Hz fox/hunter loops; the back buffer
// removes that. If the sprite can't be allocated we fall back to direct draw.

namespace {

constexpr int W = 240;
constexpr int H = 135;

M5Canvas cv(&M5.Display);
bool      cv_ok = false;

// All drawing targets the off-screen canvas; push() blits it in one shot. Using
// `auto&` at the call sites binds to M5Canvas directly (no common-base gymnastics
// between the sprite and the panel). If the sprite failed to allocate, draws are
// harmless no-ops on the empty surface.
M5Canvas& gfx() { return cv; }
void      push() { if (cv_ok) cv.pushSprite(0, 0); }

int rssi_to_px(float dbm) {
    const float lo = -110.0f, hi = -40.0f;
    if (dbm < lo) dbm = lo;
    if (dbm > hi) dbm = hi;
    return (int)((dbm - lo) / (hi - lo) * (float)W);
}

// Draw a small battery glyph with a fill proportional to charge, plus a "NN%"
// label to its left, anchored at the top-right corner. Returns the x of the
// left edge of the whole widget so the header can keep its right-text clear of
// it. When charge is unknown the body is drawn empty.
int draw_battery() {
    auto& d = gfx();
    const int bw = 26, bh = 13, ty = 3;      // body w/h, top y
    const int nub = 3;
    const int bx = W - bw - nub;             // body left x (nub sticks out right)
    int pct = battery::percent();

    uint16_t col = TFT_GREEN;
    if (pct >= 0 && pct <= 20) col = TFT_RED;
    else if (pct >= 0 && pct <= 40) col = TFT_YELLOW;

    d.drawRect(bx, ty, bw, bh, col);
    d.fillRect(bx + bw, ty + 3, nub, bh - 6, col);
    if (pct > 0) {
        int fw = (bw - 4) * pct / 100;
        if (fw > 0) d.fillRect(bx + 2, ty + 2, fw, bh - 4, col);
    }
    if (battery::charging()) {                // a small "+" mark when charging
        int cx = bx + bw / 2, cy = ty + bh / 2;
        d.drawFastHLine(cx - 3, cy, 7, TFT_BLACK);
        d.drawFastVLine(cx, cy - 3, 7, TFT_BLACK);
    }

    // "NN%" label to the left of the glyph (size 1, green).
    char lbl[8];
    if (pct >= 0) snprintf(lbl, sizeof(lbl), "%d%%", pct);
    else          snprintf(lbl, sizeof(lbl), "--");
    int lw = (int)strlen(lbl) * 6;
    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(bx - lw - 4, ty + 3);
    d.print(lbl);
    return bx - lw - 4;
}

void header(const char* left, const char* right) {
    auto& d = gfx();
    int batt_left = draw_battery();
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print(left);
    if (right && right[0]) {
        int w = (int)strlen(right) * 12;       // 6px * size 2
        d.setCursor(batt_left - w - 6, 2);     // keep clear of the battery widget
        d.print(right);
    }
}

} // namespace

namespace display {

void begin() {
    cardputer_m5_begin();
    M5.Display.setRotation(1);              // 240x135 landscape
    M5.Display.fillScreen(TFT_BLACK);
    cv.setColorDepth(16);
    cv_ok = cv.createSprite(W, H);
    gfx().setTextColor(TFT_GREEN, TFT_BLACK);
    gfx().setTextSize(2);
}

void menu(const char* const* items, int n, int sel) {
    auto& d = gfx();
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(0, 2);
    d.print("Select mode:");
    for (int i = 0; i < n; i++) {
        int y = 30 + i * 24;
        if (i == sel) {
            d.fillRect(0, y - 2, W, 22, TFT_GREEN);
            d.setTextColor(TFT_BLACK, TFT_GREEN);
        } else {
            d.setTextColor(TFT_GREEN, TFT_BLACK);
        }
        d.setCursor(4, y);
        d.print(items[i]);
    }
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    push();
}

void fox(uint16_t seq, const char* msg, bool tone_on, const char* pwr) {
    auto& d = gfx();
    d.fillScreen(TFT_BLACK);
    char pbuf[12];
    snprintf(pbuf, sizeof(pbuf), "PWR %s", pwr);
    header(tone_on ? "FOX TX *" : "FOX TX", pbuf);

    // Message wrapped at 20 cols (size 2), up to 3 lines.
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    char line[21];
    size_t len = strlen(msg);
    for (int row = 0; row < 3 && (size_t)(row * 20) < len; row++) {
        strncpy(line, msg + row * 20, 20);
        line[20] = 0;
        d.setCursor(0, 34 + row * 20);
        d.print(line);
    }

    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    char buf[20];
    snprintf(buf, sizeof(buf), "seq=%u", seq);
    d.setCursor(0, H - 10);
    d.print(buf);
    push();
}

void hunter(const char* text, float freq_mhz, int station_id, bool ditdah,
            float rssi_dbm, bool rssi_valid, bool tone_on) {
    auto& d = gfx();
    d.fillScreen(TFT_BLACK);

    char hdr[24];
    if (station_id >= 0) snprintf(hdr, sizeof(hdr), "RECV %d", station_id);
    else                 snprintf(hdr, sizeof(hdr), "RECV ---");
    header(hdr, tone_on ? "*" : "");

    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%.1f MHz", freq_mhz);
    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(W - (int)strlen(fbuf) * 6, 24);
    d.print(fbuf);
    d.setCursor(0, 24);
    d.print(ditdah ? "./-" : "TXT");

    // Last ~20 glyphs of recent copy.
    const size_t SHOW = 20;
    size_t len = strlen(text);
    const char* tail = len > SHOW ? text + (len - SHOW) : text;
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 48);
    d.print(tail);

    // RSSI bar across the bottom.
    int by = H - 18;
    d.drawRect(0, by, W, 16, TFT_GREEN);
    if (rssi_valid) d.fillRect(0, by, rssi_to_px(rssi_dbm), 16, TFT_GREEN);
    push();
}

void livekey(uint16_t seq, bool tone_on) {
    auto& d = gfx();
    d.fillScreen(TFT_BLACK);
    header(tone_on ? "LIVE KEY *" : "LIVE KEY", "");
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(0, 40);
    d.print("keying on air");
    d.setTextSize(1);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    char buf[20];
    snprintf(buf, sizeof(buf), "seq=%u", seq);
    d.setCursor(0, H - 10);
    d.print(buf);
    push();
}

void status(const char* title, const char* line1, const char* line2) {
    auto& d = gfx();
    d.fillScreen(TFT_BLACK);
    d.setTextSize(2);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    if (title) { d.setCursor(0, 8);  d.print(title); }
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    if (line1) { d.setCursor(0, 50); d.print(line1); }
    if (line2) { d.setCursor(0, 80); d.print(line2); }
    push();
}

} // namespace display

#endif // DEVICE_CARDPUTER_ADV
