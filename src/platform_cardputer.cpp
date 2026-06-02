#ifdef DEVICE_CARDPUTER_ADV

#include "platform_cardputer.h"
#include "pins.h"
#include <M5Unified.h>

void cardputer_m5_begin() {
    static bool done = false;
    if (done) return;
    done = true;

    auto cfg = M5.config();
    M5.begin(cfg);                 // brings up LCD (M5.Display) + ES8311 (M5.Speaker)
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);     // fox/live-key run full; hunter rides RSSI

    // (Re)bring up the internal I2C bus (GPIO8/9) that the TCA8418 keyboard
    // (0x34) and ES8311 codec sit on. M5.begin() is supposed to do this, and it
    // does detect the board correctly as CardputerADV — but the keypad still
    // NACKs (every register reads 0x00, keyboard dead) until the bus is begun
    // again here, after M5.begin() returns. Likely an init-order quirk inside
    // M5.begin() (the bus is set up before the board type is finalised). The
    // pins match M5's own CardputerADV table (SCL=9, SDA=8). Idempotent.
    M5.In_I2C.begin(I2C_NUM_1, PIN_I2C_SDA, PIN_I2C_SCL);
}

#endif // DEVICE_CARDPUTER_ADV
