#include <Arduino.h>
#include "pins.h"
#include "sidetone.h"
#include "morsekey.h"

// Stage 2 local-keyer test.
//
// Telegraph key -> debounced GPIO -> local sidetone. No radio: this is the
// straight-key practice loop. Key down should produce tone with no
// perceptible latency; key up silences it.
//
// Wiring: telegraph key between PIN_KEY and GND (internal pull-up).
//         sidetone on PIN_SIDETONE -> PAM8403 -> speaker (see pins.h).

static constexpr uint32_t TONE_HZ = 600;

static MorseKey key;

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.printf("# local keyer: key on GPIO %d, sidetone %lu Hz on GPIO %d\n",
                  PIN_KEY, (unsigned long)TONE_HZ, PIN_SIDETONE);

    sidetone_init(PIN_SIDETONE, TONE_HZ);
    key.begin(PIN_KEY);
}

void loop() {
    key.update();

    static bool last = false;
    bool down = key.down();
    if (down != last) {
        last = down;
        if (down) sidetone_on();
        else      sidetone_off();
        Serial.println(down ? "key down" : "key up");
    }
}
