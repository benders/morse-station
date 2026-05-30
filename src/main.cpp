#include <Arduino.h>
#include "sidetone.h"

// Stage 1 sidetone bring-up test.
//
// Drives a PAM8403 amp -> 4 ohm speaker from a single GPIO. Plays a repeating
// pattern so you can confirm a clean 600 Hz tone and set volume on the amp's
// onboard pot. No radio, no OLED — audio path only.
//
// Wiring (confirm GPIO 7 against the board silkscreen before powering the amp):
//   GPIO 7  --[~1k series R]--+--[100nF]--> PAM8403 L_IN (or R_IN)
//                            (RC smooths the square wave a little)
//   Heltec 3V3 / 5V  -> PAM8403 VCC (2.5-5 V)
//   Heltec GND       -> PAM8403 GND (common)
//   PAM8403 L_OUT +/- -> 4 ohm speaker
//
// The previous RSSI band scanner lives in git history (initial commit) if you
// need it back.

static constexpr int      PIN_SIDETONE = 7;
static constexpr uint32_t TONE_HZ      = 600;

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.printf("# sidetone test: %lu Hz on GPIO %d\n",
                  (unsigned long)TONE_HZ, PIN_SIDETONE);

    sidetone_init(PIN_SIDETONE, TONE_HZ);
}

void loop() {
    // Three short beeps (dit-like, 150 ms) then a longer one (450 ms),
    // with a 1 s gap — easy to recognize and to check for clean on/off.
    for (int i = 0; i < 3; i++) {
        Serial.println("beep (short)");
        sidetone_on();  delay(150);
        sidetone_off(); delay(150);
    }
    Serial.println("beep (long)");
    sidetone_on();  delay(450);
    sidetone_off();

    delay(1000);
}
