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
constexpr uint8_t  MAGIC          = 0x4D;   // 'M' — quick sanity/version byte

struct __attribute__((packed)) KeyState {
    uint8_t  magic;       // MAGIC
    uint8_t  station_id;
    uint8_t  key_down;    // 0/1
    uint16_t seq;         // wraps; for loss/ordering diagnostics
};

constexpr size_t PACKET_LEN = sizeof(KeyState);   // 5 bytes

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

} // namespace proto
