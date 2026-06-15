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

// Listen beacon (instructor handshake): a fox transmits this once, the instant
// it opens its inter-message RX window, to announce "I am listening NOW for
// window_ms". It replaces the instructor's silence-inference (waiting out
// CTRL_SILENCE_MS to guess the window) with an explicit trigger, so the burst
// fires immediately and the whole window is usable. Silence-sync remains the
// fallback when the beacon is lost. Distinguished by MAGIC_LISTEN so hunters
// (which only match MAGIC/MAGIC_IDENT/MAGIC_EDGE) ignore it. window_ms lets the
// instructor bound the burst to the announced deadline.
constexpr uint8_t MAGIC_LISTEN = 0x4C;   // 'L' — fox -> "entering RX window now"

struct __attribute__((packed)) Listen {
    uint8_t  magic;        // MAGIC_LISTEN
    uint8_t  station_id;   // the fox opening its window
    uint16_t window_ms;    // how long the window stays open from this packet
};

constexpr size_t LISTEN_LEN = sizeof(Listen);   // 4 bytes

inline size_t encode_listen(uint8_t station_id, uint16_t window_ms, uint8_t* buf) {
    buf[0] = MAGIC_LISTEN;
    buf[1] = station_id;
    buf[2] = (uint8_t)(window_ms & 0xFF);
    buf[3] = (uint8_t)(window_ms >> 8);
    return LISTEN_LEN;
}

inline bool decode_listen(const uint8_t* buf, size_t len, Listen& l) {
    if (len < LISTEN_LEN || buf[0] != MAGIC_LISTEN) return false;
    l.magic      = buf[0];
    l.station_id = buf[1];
    l.window_ms  = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    return true;
}

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

// Remote control (docs: instructor station). An "instructor" node (the
// Cardputer) reaches a distant fox over GFSK — not just short-range BLE — to
// re-tune it live during an exercise (new clue text, speed, power). The control
// packet carries an ASCII command line identical to what the BLE/serial console
// accepts; the receiver runs it through the SAME handle_setup_command() parser.
// The instructor is identified by the reserved source id INSTRUCTOR_ID (0), and
// a receiver only honours control packets from that id. A target_id of
// BROADCAST_ID (255) addresses every station; otherwise it must match the
// receiver's station_id. Distinguished by MAGIC_CTRL so other packet parsers
// ignore it. Variable length: a 4-byte header + a NUL-terminated command line.
constexpr uint8_t MAGIC_CTRL    = 0x43;   // 'C' — instructor -> station command
constexpr uint8_t MAGIC_CACK    = 0x41;   // 'A' — station -> instructor ack
constexpr uint8_t INSTRUCTOR_ID = 0;      // reserved source id of the instructor
constexpr size_t  CTRL_CMD_MAX  = 100;    // >= config FOX_MSG_MAX (96) + "msg "
constexpr size_t  CTRL_HDR_LEN  = 4;      // magic, src, target, seq
constexpr size_t  ACK_STATUS_MAX = 48;    // short confirmation text in an ack

struct ControlCmd {
    uint8_t src_id;                  // must be INSTRUCTOR_ID to be honoured
    uint8_t target_id;              // recipient station_id, or BROADCAST_ID
    uint8_t seq;                    // command id (wraps); dedup + ack matching
    char    cmd[CTRL_CMD_MAX + 1];  // NUL-terminated ASCII command line
};

struct ControlAck {
    uint8_t src_id;                    // responding station's id
    uint8_t target_id;                // who the ack is for (the instructor, 0)
    uint8_t seq;                      // echoes the ControlCmd.seq being answered
    char    status[ACK_STATUS_MAX + 1]; // NUL-terminated short confirmation text
};

// Encode a control command. Writes CTRL_HDR_LEN + strlen(cmd) bytes (the command
// is truncated to CTRL_CMD_MAX). Returns the byte count written.
inline size_t encode_ctrl(uint8_t src_id, uint8_t target_id, uint8_t seq,
                          const char* cmd, uint8_t* buf) {
    buf[0] = MAGIC_CTRL;
    buf[1] = src_id;
    buf[2] = target_id;
    buf[3] = seq;
    size_t i = 0;
    for (; cmd && cmd[i] && i < CTRL_CMD_MAX; ++i) buf[CTRL_HDR_LEN + i] = (uint8_t)cmd[i];
    return CTRL_HDR_LEN + i;
}

inline bool decode_ctrl(const uint8_t* buf, size_t len, ControlCmd& c) {
    if (len < CTRL_HDR_LEN || buf[0] != MAGIC_CTRL) return false;
    c.src_id    = buf[1];
    c.target_id = buf[2];
    c.seq       = buf[3];
    size_t body = len - CTRL_HDR_LEN;
    if (body > CTRL_CMD_MAX) body = CTRL_CMD_MAX;
    for (size_t i = 0; i < body; ++i) c.cmd[i] = (char)buf[CTRL_HDR_LEN + i];
    c.cmd[body] = '\0';
    return true;
}

// Encode an ack. Writes CTRL_HDR_LEN + strlen(status) bytes (status truncated to
// ACK_STATUS_MAX). target_id is the instructor that the ack answers.
inline size_t encode_ack(uint8_t src_id, uint8_t target_id, uint8_t seq,
                         const char* status, uint8_t* buf) {
    buf[0] = MAGIC_CACK;
    buf[1] = src_id;
    buf[2] = target_id;
    buf[3] = seq;
    size_t i = 0;
    for (; status && status[i] && i < ACK_STATUS_MAX; ++i) buf[CTRL_HDR_LEN + i] = (uint8_t)status[i];
    return CTRL_HDR_LEN + i;
}

inline bool decode_ack(const uint8_t* buf, size_t len, ControlAck& a) {
    if (len < CTRL_HDR_LEN || buf[0] != MAGIC_CACK) return false;
    a.src_id    = buf[1];
    a.target_id = buf[2];
    a.seq       = buf[3];
    size_t body = len - CTRL_HDR_LEN;
    if (body > ACK_STATUS_MAX) body = ACK_STATUS_MAX;
    for (size_t i = 0; i < body; ++i) a.status[i] = (char)buf[CTRL_HDR_LEN + i];
    a.status[body] = '\0';
    return true;
}

// Instructor broadcast banner (docs/plan-instructor-broadcast.md). A human-
// readable message the instructor pushes to the screen of EVERY station —
// "RETURN TO BASE", "FOX QSY", a hint or safety call — regardless of mode. This
// is distinct from MAGIC_CTRL: that carries a console command line a station
// runs silently through handle_setup_command(); this carries display text shown
// verbatim on the panel. It is always all-stations (no target_id), fire-and-
// forget (no ack — acking from every node would flood the channel), and made
// reliable by repeating the same seq a few times (see BCAST_REPEATS in main.cpp).
// Only src_id == INSTRUCTOR_ID is honoured (same threat model as MAGIC_CTRL).
// Distinguished by MAGIC_BCAST so every other parser drops it; an older node
// that predates it ignores it for free (each decode_* is magic-gated).
constexpr uint8_t MAGIC_BCAST     = 0x42;   // 'B' — instructor broadcast banner
constexpr size_t  BCAST_HDR_LEN   = 4;      // magic, src, seq, flags
constexpr size_t  BCAST_TEXT_MAX  = 40;     // two short OLED lines; airtime budget
constexpr uint8_t BCAST_FLAG_ALERT = 0x01;  // bit0: force-wake a blanked panel

struct BroadcastMsg {
    uint8_t src_id;                       // must be INSTRUCTOR_ID (0) to be honoured
    uint8_t seq;                          // wraps; for dedup (no ack)
    uint8_t flags;                        // BCAST_FLAG_*; unknown bits ignored
    char    text[BCAST_TEXT_MAX + 1];     // NUL-terminated ASCII
};

// Encode a broadcast. Writes BCAST_HDR_LEN + strlen(text) bytes (text truncated
// to BCAST_TEXT_MAX). Returns the byte count written.
inline size_t encode_bcast(uint8_t src_id, uint8_t seq, uint8_t flags,
                           const char* text, uint8_t* buf) {
    buf[0] = MAGIC_BCAST;
    buf[1] = src_id;
    buf[2] = seq;
    buf[3] = flags;
    size_t i = 0;
    for (; text && text[i] && i < BCAST_TEXT_MAX; ++i)
        buf[BCAST_HDR_LEN + i] = (uint8_t)text[i];
    return BCAST_HDR_LEN + i;
}

inline bool decode_bcast(const uint8_t* buf, size_t len, BroadcastMsg& b) {
    if (len < BCAST_HDR_LEN || buf[0] != MAGIC_BCAST) return false;
    b.src_id = buf[1];
    b.seq    = buf[2];
    b.flags  = buf[3];
    size_t body = len - BCAST_HDR_LEN;
    if (body > BCAST_TEXT_MAX) body = BCAST_TEXT_MAX;
    for (size_t i = 0; i < body; ++i) b.text[i] = (char)buf[BCAST_HDR_LEN + i];
    b.text[body] = '\0';
    return true;
}

} // namespace proto
