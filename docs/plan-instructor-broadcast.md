# Plan: Instructor Broadcast Message

Status: IMPLEMENTED on branch `feat/instructor-broadcast` (B1–B7). Firmware: the
`MAGIC_BCAST` packet in `src/protocol.h`, the `bcast [-a] <text>` / `bcast clear`
console verb, the TX campaign in `loop_instructor()`, and the banner overlay in
every mode's draw (`src/main.cpp`). B8 (Listen-beacon latency) remains deferred
until `MAGIC_LISTEN` is on `main`. Built clean on all five board envs.

## 1. Goal

Let the instructor push a short plaintext message that appears on the screen of
**every** station in the exercise — Fox, Hunter, and Instructor alike — regardless
of mode. Use case: "RETURN TO BASE", "BREAK FOR LUNCH", "FOX MOVING", a hint, or a
safety call. This is a *human-readable banner*, distinct from the existing `relay`
mechanism which sends a *console command* that a station executes silently.

Key difference from `relay`/`MAGIC_CTRL`:

| | `MAGIC_CTRL` (existing) | broadcast (this plan) |
|---|---|---|
| payload | console command line | display text |
| effect on receiver | runs `handle_setup_command()` | shows text on the OLED |
| audience | one station, or all, that run the firmware | all stations |
| ack | yes (`MAGIC_CACK`) | no (fire-and-forget) |
| visible to operator | only the command's side effects | the literal message |

A broadcast could *technically* be done today with `relay 255 <some-display-cmd>`,
but there is no console command that paints arbitrary text on the panel, and a
broadcast `MAGIC_CTRL` would (a) demand an ack from every node, flooding the
channel, and (b) route through the full command parser. A dedicated, ack-free,
display-only packet is cleaner and safer.

## 2. Packet format

### 2.1 Magic byte

Existing magic bytes (all ASCII, `src/protocol.h`):

| const | value | char |
|---|---|---|
| `MAGIC` | `0x4D` | `'M'` KeyState |
| `MAGIC_IDENT` | `0x49` | `'I'` Ident |
| `MAGIC_EDGE` | `0x45` | `'E'` EdgeEvent |
| `MAGIC_CTRL` | `0x43` | `'C'` control command |
| `MAGIC_CACK` | `0x41` | `'A'` control ack |

New, collision-free:

```cpp
constexpr uint8_t MAGIC_BCAST = 0x42;   // 'B' — instructor broadcast banner
```

`0x42` is unused and mnemonic ('B' for Broadcast). Any older node that does not
recognise it falls through every `decode_*` guard (each checks `buf[0] != MAGIC_*`)
and drops the frame — see §5.

### 2.2 Layout

Mirror the variable-length `MAGIC_CTRL` style (4-byte header + NUL-terminated body)
so the encode/decode helpers and buffer-sizing patterns are familiar:

```cpp
constexpr size_t  BCAST_HDR_LEN  = 4;     // magic, src, seq, flags
constexpr size_t  BCAST_TEXT_MAX = 40;    // see airtime budget §2.4

struct BroadcastMsg {
    uint8_t src_id;                       // must be INSTRUCTOR_ID (0) to be honoured
    uint8_t seq;                          // wraps; for dedup (no ack)
    uint8_t flags;                        // bit0 = alert (force-wake panel); rest reserved
    char    text[BCAST_TEXT_MAX + 1];     // NUL-terminated ASCII
};
```

Wire bytes:

| offset | field | size |
|---|---|---|
| 0 | `MAGIC_BCAST` (`0x42`) | 1 |
| 1 | `src_id` | 1 |
| 2 | `seq` | 1 |
| 3 | `flags` | 1 |
| 4..4+L-1 | `text` (L = strlen, ≤ `BCAST_TEXT_MAX`) | L |

Total on-air length = `BCAST_HDR_LEN + L` = **4..44 bytes**. There is no
`target_id`: a broadcast banner is always all-stations, so the `BROADCAST_ID`
addressing of `MAGIC_CTRL` is omitted. (If a future "private note to one station"
is wanted, add a `target_id` byte then; out of scope here.)

`flags` is included up front so an "alert" banner can force a blanked panel awake
while an ordinary banner respects the operator's blank state (§4.3). Defining the
byte now avoids a wire-format bump later.

### 2.3 Encode / decode helpers

Same shape as `encode_ctrl` / `decode_ctrl` in `protocol.h`:

```cpp
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
```

### 2.4 Max text length vs. GFSK airtime budget

Radio config (`src/radio.cpp`): GFSK, `BITRATE_KBPS = 4.8f`, `FREQ_DEV_KHZ = 5.0`,
RX BW default `23.4 kHz`. At 4.8 kbps the payload byte time is:

```
1 byte = 8 bits / 4800 bps = 1.667 ms
```

A max broadcast frame is `BCAST_HDR_LEN(4) + BCAST_TEXT_MAX(40)` = **44 payload
bytes** → `44 * 1.667 ms ≈ 73 ms` of *payload* airtime. Add the RadioLib FSK
overhead (preamble + 2-byte sync + length byte + 2-byte CRC, the same framing the
existing `msg` command rides on). Using the comment in `main.cpp` as the
calibration anchor — *"at 4.8 kbps a ~100-byte `msg` command is ~200 ms on air"* —
the per-byte on-air figure including overhead is ≈ 2 ms/byte, so a 44-byte
broadcast is **≈ 90 ms on air**.

Why 40 and not more:

- It must fit comfortably inside the same silence window a `MAGIC_CTRL` `msg`
  command uses. The fox's RX window (`CTRL_RX_WINDOW_MS = 2500 ms`) and the hunter
  presence timeout easily absorb 90 ms; a 100-byte (≈200 ms) frame already works
  today, so 90 ms is conservative.
- 40 chars is two short OLED lines of human text — enough for "RETURN TO BASE NOW"
  or "FOX QSY 906.000". It is well under `FOX_MSG_MAX (96)` / `CTRL_CMD_MAX (100)`,
  keeping the banner visibly distinct from a full fox-message edit and minimising
  channel occupancy for a packet that will be **repeated** (§3.2).
- Display reality: `display::status()` renders a title + two body lines; ~16-20
  chars/line on the 128px OLED. 40 chars maps to roughly the two body lines.

Total worst case (max text, repeated `BCAST_REPEATS` times, see §3.2) at e.g. 5
repeats ≈ 5 × 90 ms = 450 ms of channel time spread over the burst window — a small
fraction of one fox message cycle.

## 3. Transport

### 3.1 Riding the existing instructor burst path

The broadcast reuses the proven instructor staging + burst machinery in
`main.cpp` rather than inventing new timing:

- A new console verb (`bcast <text...>`, §6 B3) stages the text into the existing
  `g_ctrl`-style state. Cleanest: add a `g_ctrl.kind` discriminator (CMD vs BCAST)
  and a `g_ctrl.text` field, OR add a parallel `g_bcast` struct mirroring the
  control struct. The parallel struct is lower-risk (no change to the working
  `relay` path) and is the recommended approach.
- `loop_instructor()` already runs the staging → burst → window-expiry lifecycle.
  Extend it (or a sibling `loop_instructor_bcast`) to, when a broadcast is staged,
  emit `MAGIC_BCAST` frames using the **same** scheduling primitives:
  - It is always all-stations, so it takes the **broadcast/probe path** that
    already exists: `burst_now = (now - last_tx >= CTRL_PROBE_INTERVAL)`
    (`CTRL_PROBE_INTERVAL = 1500 ms`). There is no single fox phase to silence-sync
    to for an all-stations message — the same reason the existing code falls back
    to the sparse probe for `BROADCAST_ID` commands.
  - Bound the campaign by repeat count rather than the 90 s `CTRL_BURST_WINDOW`
    (see §3.2); broadcasts are fire-and-forget so there is no ack to wait for.

### 3.2 Reliability: repeats, not acks

GFSK here is fire-and-forget (the module docstring at the top of `protocol.h`:
*"No ACKs, no retries — a dropped packet is corrected by the next one."*). A banner
is a one-shot event with no natural "next packet" to self-heal it, so:

- **No ack.** Acking from every node would flood the channel (the exact reason the
  existing broadcast `MAGIC_CTRL` path is a sparse probe, not lockstep). Receivers
  stay silent.
- **Fixed small repeat count** for delivery probability. Define:

  ```cpp
  constexpr uint32_t BCAST_REPEATS = 5;   // burst the same seq this many times
  ```

  Emit the same `seq` `BCAST_REPEATS` times, spaced `CTRL_PROBE_INTERVAL (1500 ms)`
  apart, so the campaign spans ~7.5 s — long enough to land in several fox RX
  windows and hunter idle gaps, short enough not to dominate the channel. After the
  last repeat the instructor returns to its idle "ready" status.
- **Receiver dedup by `seq`** (same pattern as `control_rx_try`'s
  `last_seq`/`have_last`): apply/show a given `seq` once; ignore further copies so
  the banner does not re-trigger or reset its own dismissal timer on each repeat.
  Use a *per-source* last-seq (only `INSTRUCTOR_ID` is honoured, so a single static
  is fine).

### 3.3 Listen-beacon note

The end-of-message **Listen beacon** (`MAGIC_LISTEN`) referenced in project memory
lives on the `power-ble-follows-panel`/instructor branch, **not on `main`** (a
`grep` for `MAGIC_LISTEN` on this branch finds only the unrelated
`CTRL_ACK_LISTEN_MS` constant). When that branch merges, the broadcast can opt into
the same optimisation: a fox that has just emitted its Listen beacon is known to be
entering an RX window, so the instructor can fire a broadcast repeat immediately
instead of waiting out `CTRL_PROBE_INTERVAL`. This is a *latency* improvement only;
the design above is complete and correct without it. Implement against the
probe-interval path first (B4), then layer the beacon trigger if/when available.

## 4. Rendering per mode

All modes already own a per-loop draw and a `display::status(title, l1, l2)` helper
(`display.h`). A broadcast is shown as an **overlay banner** that temporarily
replaces the normal mode screen.

### 4.1 Shared banner state

Add file-scope state in `main.cpp`, set by the receive handler, consumed by each
mode's draw:

```cpp
static char     g_banner[proto::BCAST_TEXT_MAX + 1] = {0};
static uint32_t g_banner_until = 0;   // millis() deadline; 0 = no banner
```

A small helper `banner_active(now)` returns `g_banner_until && now < g_banner_until`.

### 4.2 What each mode shows

The text is split across the two body lines of `display::status()`:

- **Fox** (`loop_fox`): while `banner_active`, draw
  `display::status("INSTRUCTOR", line1, line2)` instead of the normal
  `display::fox(...)` TX status. Keying/TX continues underneath; only the *panel*
  is borrowed. After timeout, the next draw reverts to `display::fox(...)`.
- **Hunter** (`loop_hunter`): same overlay, replacing `display::hunter(...)`.
  Decode/copy continues underneath; the banner just occupies the glass.
- **Instructor** (`loop_instructor`): the instructor sees its *own* broadcast too
  (it should — confirms what went out). While bursting, show
  `display::status("Instructor TX", "BCAST seq N", "<text>")` reusing the existing
  TX status line; the receiver-side overlay path is a no-op on the instructor since
  it does not RX its own send, so set `g_banner` locally when staging.
- **Live-key** (`loop_livekey`): overlay over `display::livekey(...)` identically.

A single `if (banner_active(now)) { draw banner; } else { /* normal draw */ }`
guard at the top of each mode's draw section keeps this uniform and small.

### 4.3 Idle-blank interaction

`display.h`: the panel blanks after `IDLE_BLANK_MS` (60 s) of no `activity()`, and
any `activity()` re-powers it. Behaviour by `flags`:

- **Ordinary banner** (`flags` bit0 = 0): set `g_banner`/`g_banner_until` but do
  **not** call `display::activity()`. If the panel is awake the banner shows; if the
  operator has let it blank, it stays blank (respect their battery/eyes choice).
  When they next wake the panel (button/keypress) within the banner window, they
  see it.
- **Alert banner** (`flags` bit0 = 1, set by `bcast -a <text>`): call
  `display::activity()` on receipt to force-wake a blanked panel, exactly as inbound
  hunter keying does today (`loop_hunter` calls `display::activity()` on RX). This
  is the "RETURN TO BASE" / safety path that must be seen.

### 4.4 Dismiss / timeout

- **Timeout**: `g_banner_until = now + BCAST_SHOW_MS`. Suggested
  `BCAST_SHOW_MS = 15000` (15 s) — long enough to read two lines, short enough that
  it clears itself with no operator action. A fresh broadcast (`seq` changes via
  dedup) replaces the text and re-arms the deadline.
- **Manual dismiss**: a button tap / keypress (the existing `prg_tapped()` /
  Cardputer `keyboard::read_char`) while a banner is showing clears it
  (`g_banner_until = 0`) instead of doing its normal action, and is swallowed —
  mirroring the existing "wake press is swallowed" pattern. Optional but cheap; can
  land in B6.

## 5. Backward compatibility

Older firmware that predates `MAGIC_BCAST` ignores it **for free**:

- Every existing parser is magic-gated: `decode()` checks `buf[0] != MAGIC`,
  `decode_ident` checks `MAGIC_IDENT`, `decode_edge` `MAGIC_EDGE`, `decode_ctrl`
  `MAGIC_CTRL`, `decode_ack` `MAGIC_CACK`. A `0x42` first byte matches none, so each
  returns `false`/drops the frame.
- The RX dispatch in `main.cpp` (the `control_rx_try` / keystate / edge / ident
  cascade) tries each decoder and falls through on an unknown magic; an unrecognised
  `0x42` frame is simply not consumed by any handler and is discarded. No crash, no
  misparse (the lengths differ from `PACKET_LEN`/`IDENT_LEN`/`EDGE_LEN` and the
  magic differs, so no accidental match).
- A new node receiving *old* traffic is unaffected: it only ever shows a banner on a
  valid `MAGIC_BCAST` from `INSTRUCTOR_ID`.
- Forward-compat within the new format: receivers must ignore unknown `flags` bits
  (mask only bit0) and tolerate `text` shorter/longer than expected (decode already
  clamps to `BCAST_TEXT_MAX`). This lets a later `target_id` or richer flags ship
  without breaking these receivers, provided the header stays 4 bytes — if a 5th
  header byte is ever needed, bump the magic.

Security/abuse note (same model as `MAGIC_CTRL`): only `src_id == INSTRUCTOR_ID (0)`
is honoured, so a stray Hunter/Fox cannot paint banners on the field. This is
spoofable on an open channel — acceptable for a training exercise, same threat model
as the existing control path.

## 6. Implementation phase breakdown

Small, individually reviewable steps:

- **B1 — Protocol.** Add `MAGIC_BCAST`, `BCAST_HDR_LEN`, `BCAST_TEXT_MAX`,
  `struct BroadcastMsg`, `encode_bcast`, `decode_bcast` to `src/protocol.h`. Pure
  header, no behaviour change. (Mirrors the `MAGIC_CTRL` block.)

- **B2 — Receive + dedup.** In `main.cpp`, add `broadcast_rx_try(buf, n, now)`
  mirroring `control_rx_try`: decode, require `src_id == INSTRUCTOR_ID`, dedup by
  `seq`, on fresh copy populate `g_banner` / `g_banner_until` and, if alert flag,
  `display::activity()`. Wire it into the same RX cascade as `control_rx_try` (call
  it before/alongside, return-true to stop further parsing). No TX yet — testable by
  hand-injecting a frame.

- **B3 — Console verb.** Add `bcast <text>` (and `bcast -a <text>` for alert) to
  `handle_setup_command()`, Instructor-mode only (same guard as `relay`). Stage into
  a new `g_bcast` struct (kind, seq from NVS-incremented counter like `g_ctrl.seq`,
  flags, text, start_ms, repeats_left). Also set the local `g_banner` so the
  instructor sees its own message. Help text + `show` line.

- **B4 — Transmit campaign.** In `loop_instructor()` (or a sibling), when
  `g_bcast.active`, burst `MAGIC_BCAST` every `CTRL_PROBE_INTERVAL`, decrementing
  `repeats_left` from `BCAST_REPEATS`; clear active when exhausted. Reuse
  `radio::send()` + `radio::start_receive()`. Show "Instructor TX / BCAST seq N /
  text" while active.

- **B5 — Render overlay.** Add `banner_active(now)` and the
  `if (banner_active) draw-banner else normal` guard to `loop_fox`, `loop_hunter`,
  `loop_livekey` (and the instructor idle draw). Respect/override idle-blank per
  `flags` (§4.3).

- **B6 — Dismiss + polish.** Button/keypress dismiss while a banner shows (swallow
  the press). Tune `BCAST_SHOW_MS`, `BCAST_REPEATS`. Optional `bcast clear`
  broadcast (text empty → receivers clear `g_banner_until`).

- **B7 — Docs + console help.** Update `docs/` and the console help string / any
  `commands.md`. Note the field test: instructor `bcast` → confirm banner on a Fox
  and a Hunter, confirm old-firmware node ignores it (no crash), confirm alert flag
  wakes a blanked panel.

- **B8 (deferred) — Listen-beacon latency.** Once `MAGIC_LISTEN` merges to `main`,
  trigger an immediate broadcast repeat on hearing a fox's Listen beacon (§3.3).

## 7. Risks / open questions

- **Channel load during a banner campaign vs. live keying.** 5 × ~90 ms over 7.5 s
  is light, but verify hunter copy is not visibly disturbed when a broadcast lands
  outside a fox RX window (it can collide with the 30 ms keystate stream just like
  any probe burst). Mitigation: keep `BCAST_REPEATS` low; the keystate stream
  self-heals a single lost packet.
- **Banner vs. critical mode UI.** Borrowing the whole panel hides the
  hunter's live copy / fox TX level for `BCAST_SHOW_MS`. 15 s default is a
  deliberate trade; tune in B6. Could later render the banner as a one-line
  toast instead of a full `status()` screen.
- **`g_banner` width.** `BCAST_TEXT_MAX (40)` exceeds two OLED lines' worth of
  legible text on the smallest panel; the draw code should truncate/wrap rather than
  assume it fits. Cardputer's larger LCD (`display_cardputer.cpp`) has more room.
