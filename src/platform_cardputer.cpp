#ifdef DEVICE_CARDPUTER_ADV

#include "platform_cardputer.h"
#include <M5Unified.h>

void cardputer_m5_begin() {
    static bool done = false;
    if (done) return;
    done = true;

    auto cfg = M5.config();
    M5.begin(cfg);                 // brings up LCD (M5.Display) + ES8311 (M5.Speaker)
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);     // fox/live-key run full; hunter rides RSSI
}

#endif // DEVICE_CARDPUTER_ADV
