#pragma once
#include <stdint.h>

// Local sidetone generator. On the Heltec V4 it streams 16-bit PCM over I2S to a
// MAX98357A class-D amp -> 4 ohm speaker (gpio_pin is ignored; the I2S pin trio
// comes from pins.h). On the Cardputer ADV the same API drives the ES8311 codec.
//
// The frequency is fixed at init time; on/off just gates the output, so
// key-down/up has no retune latency. Both the key handler and (later) the RX
// decoder call sidetone_on()/sidetone_off().

void sidetone_init(int gpio_pin, uint32_t freq_hz = 600);
void sidetone_on();
void sidetone_off();

// Set tone loudness, 0 (faint) .. 255 (full). Applied on the next (or current)
// sidetone_on(). The hunter drives this from received RSSI; fox/live-key leave
// it at full. Volume is a true sample amplitude (constant timbre at every level).
void sidetone_set_volume(uint8_t vol);

// Set the playback level directly in GAIN_Q15/1024 units (1..32; 8 -> gain 8192,
// 32 -> full swing). This is the operator-facing volume knob persisted in config
// and driven by the `vol` console command — distinct from sidetone_set_volume()'s
// 0..255 RSSI curve. On the Cardputer the units map onto M5.Speaker's 0..255.
void sidetone_set_level(uint8_t units);

// Master mute. Independent of sidetone_set_volume(): when muted, sidetone_on()
// produces no sound and a tone already sounding is silenced immediately; the
// gate is remembered so unmuting resumes a held key. For a node operating near
// people. Driven by the Cardputer 'm' key and the BLE `mute` command, persisted
// in config (see config::muted()).
void sidetone_set_mute(bool muted);

// Attention gate for instructor alerts (docs/plan-alert-tone.md). Forces the
// sidetone ON at the board's fixed frequency regardless of the master mute AND
// regardless of any in-progress key/RX tone — a broadcast must be heard even on
// a node that's been muted near people. The main loop times the tone's length
// and calls sidetone_alert(false) to end it, which restores the underlying
// key/mute state. Independent of sidetone_set_mute(): it never changes the
// persisted mute, so the node is muted again the instant the alert ends.
void sidetone_alert(bool on);
