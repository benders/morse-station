#include <Arduino.h>
#include "pins.h"
#include "sidetone.h"
#include "morse.h"

// Stage 4 fox-message audio loop (no radio).
//
// Encodes the fox's location description to Morse timing and keys the local
// sidetone, looping with a pause. Validates the encoder + timing by ear
// before any radio is involved. The same Player feeds the radio keystate
// stream in Stage 5.

static constexpr uint32_t TONE_HZ      = 600;
static constexpr uint8_t  WPM          = 13;
static constexpr uint32_t REPEAT_PAUSE = 3000;   // ms between repeats
static const char* FOX_MSG = "FOX NEAR THE BIG OAK BY THE LAKE";

static morse::Player player;
static bool last_down = false;
static uint32_t pause_until = 0;

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.printf("# fox loop @ %u wpm: \"%s\"\n", WPM, FOX_MSG);

    sidetone_init(PIN_SIDETONE, TONE_HZ);
    player.begin(WPM);
    player.start(FOX_MSG);
}

void loop() {
    uint32_t now = millis();

    if (!player.finished()) {
        player.update(now);
        bool down = player.down();
        if (down != last_down) {
            last_down = down;
            if (down) sidetone_on(); else sidetone_off();
        }
        if (player.finished()) {
            sidetone_off();
            last_down = false;
            pause_until = now + REPEAT_PAUSE;
            Serial.println("# message done, pausing");
        }
    } else if (now >= pause_until) {
        Serial.println("# repeat");
        player.start(FOX_MSG);
    }
}
