#pragma once
#include <stdint.h>

// Local sidetone generator. Drives a single GPIO with a square wave via the
// ESP32 LEDC peripheral, intended to feed a PAM8403 amp -> 4 ohm speaker.
//
// The frequency is fixed at init time; on/off just gates the output, so
// key-down/up has no retune latency. Both the key handler and (later) the RX
// decoder call sidetone_on()/sidetone_off().

void sidetone_init(int gpio_pin, uint32_t freq_hz = 600);
void sidetone_on();
void sidetone_off();
