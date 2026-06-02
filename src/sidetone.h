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

// Set tone loudness, 0 (faint) .. 255 (full). Applied on the next (or current)
// sidetone_on(). The hunter drives this from received RSSI; fox/live-key leave
// it at full. Volume is the square-wave duty cycle, not a true amplitude.
void sidetone_set_volume(uint8_t vol);

// Master mute. Independent of sidetone_set_volume(): when muted, sidetone_on()
// produces no sound and a tone already sounding is silenced immediately; the
// gate is remembered so unmuting resumes a held key. For a node operating near
// people. Driven by the Cardputer 'm' key and the BLE `mute` command, persisted
// in config (see config::muted()).
void sidetone_set_mute(bool muted);
