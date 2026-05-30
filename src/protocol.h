#pragma once
#include <stdint.h>
#include <stddef.h>

// Keystate-broadcast protocol (design-notes approach A): the transmitter sends
// the current key-down/up state every TX_INTERVAL_MS. Receivers reconstruct
// timing from the stream and drive their sidetone. No ACKs, no retries — a
// dropped packet is corrected by the next one. This carries live keying and
// the canned fox loop identically.

namespace proto {

constexpr uint8_t  BROADCAST_ID   = 255;
constexpr uint32_t TX_INTERVAL_MS = 30;
constexpr uint8_t  MAGIC          = 0x4D;   // 'M' — KeyState sanity/version byte
constexpr uint8_t  MAGIC_IDENT    = 0x49;   // 'I' — station-ID packet

// Station identification cadence. A licensed "fox" running under Part 97 must
// transmit its callsign at least every 10 minutes; we send well under that.
// (Under Part 15 this packet is harmless; the callsign also rides in the keyed
// fox message text for an audible CW ID.)
constexpr uint32_t IDENT_INTERVAL_MS = 8UL * 60UL * 1000UL;   // 8 minutes

constexpr size_t   CALLSIGN_MAX = 10;       // null-padded in the packet

struct __attribute__((packed)) KeyState {
    uint8_t  magic;       // MAGIC
    uint8_t  station_id;
    uint8_t  key_down;    // 0/1
    uint16_t seq;         // wraps; for loss/ordering diagnostics
};

constexpr size_t PACKET_LEN = sizeof(KeyState);   // 5 bytes

// Dedicated station-ID packet (approach 2): carries the operator callsign,
// transmitted periodically rather than in every KeyState. Distinguished from
// KeyState by MAGIC_IDENT, so hunters that only parse KeyState simply ignore it.
struct __attribute__((packed)) Ident {
    uint8_t magic;                 // MAGIC_IDENT
    uint8_t station_id;
    char    call[CALLSIGN_MAX];    // ASCII, null-padded
};

constexpr size_t IDENT_LEN = sizeof(Ident);   // 12 bytes

// Serialize/parse. encode writes PACKET_LEN bytes. decode validates MAGIC and
// length. Both little-endian (native ESP32-S3 layout).
inline size_t encode(const KeyState& ks, uint8_t* buf) {
    buf[0] = ks.magic;
    buf[1] = ks.station_id;
    buf[2] = ks.key_down;
    buf[3] = (uint8_t)(ks.seq & 0xFF);
    buf[4] = (uint8_t)(ks.seq >> 8);
    return PACKET_LEN;
}

inline bool decode(const uint8_t* buf, size_t len, KeyState& ks) {
    if (len < PACKET_LEN || buf[0] != MAGIC) return false;
    ks.magic      = buf[0];
    ks.station_id = buf[1];
    ks.key_down   = buf[2];
    ks.seq        = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
    return true;
}

// Build a station-ID packet from a (possibly short) callsign string. Writes
// IDENT_LEN bytes: the callsign is copied up to CALLSIGN_MAX and null-padded.
inline size_t encode_ident(uint8_t station_id, const char* call, uint8_t* buf) {
    buf[0] = MAGIC_IDENT;
    buf[1] = station_id;
    for (size_t i = 0; i < CALLSIGN_MAX; ++i) {
        char c = (call && call[i]) ? call[i] : 0;
        buf[2 + i] = (uint8_t)c;
        if (!c) call = nullptr;   // pad the rest with zeros
    }
    return IDENT_LEN;
}

inline bool decode_ident(const uint8_t* buf, size_t len, Ident& id) {
    if (len < IDENT_LEN || buf[0] != MAGIC_IDENT) return false;
    id.magic      = buf[0];
    id.station_id = buf[1];
    for (size_t i = 0; i < CALLSIGN_MAX; ++i) id.call[i] = (char)buf[2 + i];
    return true;
}

} // namespace proto
