#pragma once
#include <stdint.h>

// OLED helper (SSD1315 via U8g2). Powers the Vext peripheral rail and renders
// the few screens the station needs: a boot mode menu, the fox TX status, and
// the hunter RX view (decoded text + RSSI bar).

namespace display {

void begin();

// Boot menu: items[0..n-1], sel highlighted.
void menu(const char* const* items, int n, int sel);

// Fox transmit status. pwr is a short power-level label ("HI"/"MED"/"LO").
void fox(uint16_t seq, const char* msg, bool tone_on, const char* pwr);

// Hunter receive view: a single scrolling line of recent copy (last ~16 chars),
// last-packet RSSI, tone indicator. The header shows the TX node's callsign (or
// "RECV ---" until a packet is heard) and the operating frequency. When ditdah
// is true, `text` is interpreted as a raw dit/dah element stream (learning aid)
// rather than decoded letters. rssi_valid=false dims the bar (no packet yet).
// station_id < 0 means nothing has been received yet.
void hunter(const char* text, float freq_mhz, int station_id, bool ditdah,
            float rssi_dbm, bool rssi_valid, bool tone_on);

// Instructor control view. pwr is a short power-level label; tx_active true
// while a burst is in flight.
void instructor(const char* pwr, const char* line1, const char* line2,
                bool tx_active = false);

// Live-key transmit view.
void livekey(uint16_t seq, bool tone_on);

// Generic three-line status screen (title + two body lines). Pass nullptr for
// an unused body line. Used by bring-up/test sketches. */
void status(const char* title, const char* line1, const char* line2);

// Boot splash: title + two body lines confined to the left half of the panel,
// with the OSG crest occupying the right half. Pass nullptr for an unused body
// line.
void splash(const char* title, const char* line1, const char* line2);

// Instructor broadcast banner overlay (docs/plan-instructor-broadcast.md). Shows
// an "INSTRUCTOR" title and the message text: centered when it fits one line,
// wrapped to two static lines when it fits two, and horizontally scrolled
// (marquee, driven by `now`) when it is longer than the panel can show at once.
// The caller keeps the panel awake (display::activity()) for the banner's life.
void banner(const char* text, uint32_t now);

// Idle-blanking (battery saver). The panel is the largest idle load on these
// boards (an OLED left on draws ~tens of mA; the Cardputer backlight more), so
// after IDLE_BLANK_MS with no activity tick() powers the panel down. Every wake
// source — button, serial/BLE command, hunter RX keying — calls activity(),
// which records the time and re-powers a blanked panel. tick() is called once
// per main-loop pass. blanked() exposes the state so the `screen` console
// command can verify it without a human watching the glass. Wake sources are
// wired in main.cpp's loop()/handlers.
void activity();           // note a wake event (re-powers the panel if blanked)
void tick(uint32_t now);   // per-loop: blank the panel after the idle timeout
void blank_now();          // force the panel off immediately (`screen off` test)
bool blanked();            // true while the panel is powered down (for `screen`)
uint32_t idle_ms();        // ms since the last activity() (for `screen`)

} // namespace display
