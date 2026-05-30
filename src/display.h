#pragma once
#include <stdint.h>

// OLED helper (SSD1315 via U8g2). Powers the Vext peripheral rail and renders
// the few screens the station needs: a boot mode menu, the fox TX status, and
// the hunter RX view (decoded text + RSSI bar).

namespace display {

void begin();

// Boot menu: items[0..n-1], sel highlighted.
void menu(const char* const* items, int n, int sel);

// Fox transmit status.
void fox(uint16_t seq, const char* msg, bool tone_on);

// Hunter receive view: rolling decoded text, last-packet RSSI, tone indicator.
// rssi_valid=false dims the bar (no packet yet).
void hunter(const char* text, float rssi_dbm, bool rssi_valid, bool tone_on);

// Live-key transmit view.
void livekey(uint16_t seq, bool tone_on);

} // namespace display
