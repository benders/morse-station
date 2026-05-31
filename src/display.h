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
// "----" until an Ident is heard) and the operating frequency. When ditdah is
// true, `text` is interpreted as a raw dit/dah element stream (learning aid)
// rather than decoded letters. rssi_valid=false dims the bar (no packet yet).
void hunter(const char* text, float freq_mhz, const char* call, bool ditdah,
            float rssi_dbm, bool rssi_valid, bool tone_on);

// Live-key transmit view.
void livekey(uint16_t seq, bool tone_on);

// Generic three-line status screen (title + two body lines). Pass nullptr for
// an unused body line. Used by bring-up/test sketches. */
void status(const char* title, const char* line1, const char* line2);

} // namespace display
