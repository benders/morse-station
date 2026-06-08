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

// Dedicated station-ID packet (approach 2): carries the operator callsign and
// the fox's keying speeds, transmitted periodically (and at the top of each fox
// message loop) rather than in every KeyState. Distinguished from KeyState by
// MAGIC_IDENT, so hunters that only parse KeyState simply ignore it. The speeds
// let a hunter seed its decoder thresholds to the fox's actual timing instead
// of its own config — needed for correct decode of slow/Farnsworth sending.
struct __attribute__((packed)) Ident {
    uint8_t magic;                 // MAGIC_IDENT
    uint8_t station_id;
    uint8_t wpm;                   // overall (effective) speed
    uint8_t char_wpm;              // Farnsworth character speed (>= wpm)
    char    call[CALLSIGN_MAX];    // ASCII, null-padded
};

constexpr size_t IDENT_LEN = sizeof(Ident);   // 14 bytes

// Edge-event keying (docs/edge-events.md): instead of sampling key state every
// TX_INTERVAL_MS, the transmitter sends a packet only when the key *changes*,
// carrying the TX-measured duration of the segment that just ended. The
// receiver reconstructs exact element timing from those durations rather than
// from jittered local arrival times. Distinguished by MAGIC_EDGE so legacy
// nodes (which only match MAGIC) ignore it.
constexpr uint8_t MAGIC_EDGE = 0x45;   // 'E'

// Idle heartbeat: when the key has been steady this long, re-send the current
// state as a (heartbeat) EdgeEvent. Gives presence, late-joiner re-anchor, and
// double-loss recovery without the 30 ms stream. See docs/edge-events.md.
constexpr uint32_t HEARTBEAT_MS = 700;

// flags bits in EdgeEvent.
constexpr uint8_t EDGE_FLAG_DOWN      = 0x01;  // key level entered: 1=down,0=up
constexpr uint8_t EDGE_FLAG_HEARTBEAT = 0x02;  // re-assert, not a real edge

struct __attribute__((packed)) EdgeEvent {
    uint8_t  magic;        // MAGIC_EDGE
    uint8_t  station_id;
    uint8_t  seq;          // per-edge, wraps mod 256 (loss/dup/reorder detect)
    uint8_t  flags;        // EDGE_FLAG_*
    uint16_t dur_now_ms;   // duration of the segment that JUST ENDED
    uint16_t dur_prev_ms;  // duration of the segment before that (single-loss heal)
};

constexpr size_t EDGE_LEN = sizeof(EdgeEvent);   // 8 bytes

inline size_t encode_edge(const EdgeEvent& e, uint8_t* buf) {
    buf[0] = e.magic;
    buf[1] = e.station_id;
    buf[2] = e.seq;
    buf[3] = e.flags;
    buf[4] = (uint8_t)(e.dur_now_ms & 0xFF);
    buf[5] = (uint8_t)(e.dur_now_ms >> 8);
    buf[6] = (uint8_t)(e.dur_prev_ms & 0xFF);
    buf[7] = (uint8_t)(e.dur_prev_ms >> 8);
    return EDGE_LEN;
}

inline bool decode_edge(const uint8_t* buf, size_t len, EdgeEvent& e) {
    if (len < EDGE_LEN || buf[0] != MAGIC_EDGE) return false;
    e.magic       = buf[0];
    e.station_id  = buf[1];
    e.seq         = buf[2];
    e.flags       = buf[3];
    e.dur_now_ms  = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    e.dur_prev_ms = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    return true;
}

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

// Build a station-ID packet from a (possibly short) callsign string and the
// keying speeds. Writes IDENT_LEN bytes: the callsign is copied up to
// CALLSIGN_MAX and null-padded.
inline size_t encode_ident(uint8_t station_id, const char* call,
                           uint8_t wpm, uint8_t char_wpm, uint8_t* buf) {
    buf[0] = MAGIC_IDENT;
    buf[1] = station_id;
    buf[2] = wpm;
    buf[3] = char_wpm;
    for (size_t i = 0; i < CALLSIGN_MAX; ++i) {
        char c = (call && call[i]) ? call[i] : 0;
        buf[4 + i] = (uint8_t)c;
        if (!c) call = nullptr;   // pad the rest with zeros
    }
    return IDENT_LEN;
}

inline bool decode_ident(const uint8_t* buf, size_t len, Ident& id) {
    if (len < IDENT_LEN || buf[0] != MAGIC_IDENT) return false;
    id.magic      = buf[0];
    id.station_id = buf[1];
    id.wpm        = buf[2];
    id.char_wpm   = buf[3];
    for (size_t i = 0; i < CALLSIGN_MAX; ++i) id.call[i] = (char)buf[4 + i];
    return true;
}

} // namespace proto
