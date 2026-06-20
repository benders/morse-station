# Plan — text-frame canned-message mode (field note 7, 2026-06-19)

Status: **planned, not started.** This is the design for field note §7
(`FIELD-NOTES-20260619.md`). It is a follow-on to the smaller field-note fixes
(§4 always-MAX ACK, §6 sticky alert + fox halt); do those first.

## Problem

For the **canned message** exercise (not live keying) the fox today streams the
clue as a sequence of `proto::EdgeEvent` key-up/down timing edges (8 bytes each,
`protocol.h:102`). The hunter reconstructs Morse from those edges and decodes it.
At the **edge of range** any single lost edge punches a hole that renders as `?`
(field note §5; `protocol.h:108`, "no longer heals"). A long clue is dozens of
edges, so loss probability compounds and the screen fills with `????`.

A short **text frame** carrying the whole clue as ASCII either arrives or
doesn't, and a **retransmit burst** (the pattern we already use for ACKs and
broadcasts) drives eventual delivery near-certain. The hunter renders clean Morse
*locally* from the text at the announced wpm, so audio is crisp regardless of RF
conditions and there is no decode ambiguity.

This is a **second message mode, not a replacement.** Live keying's signal *is*
the real-time edge stream — there is no text to send ahead of time — so
`EdgeEvent` stays for `MODE_LIVEKEY` and for any fox configured to key live. The
text frame is for the canned-clue fox loop only.

## Scope decisions (settled in the field note)

- **Per-exercise switch, default keeps current behaviour.** Whether the canned
  exercise trains *copying real off-air Morse* (edges) or *practicing Morse
  reading in clean conditions* (text) is a deliberate choice. Add a config flag;
  default to the existing edge/compat keying so nothing changes unless opted in.
- **Forward-compatible.** A new magic byte means hunters that only understand
  `MAGIC`/`MAGIC_IDENT`/`MAGIC_EDGE` ignore the text frame for free (every
  `decode_*` is magic-gated). Mixed fleets keep working.
- **Unify ControlCmd ⊕ text-MSG, NOT all four packet types.** Both are "addressed
  ASCII payload + seq". Converge them onto one framed-text packet with a
  `kind`/`delivery` discriminator. Keep the live-keying `EdgeEvent` stream
  separate — it is a real-time timing stream, a different problem. See
  "Protocol option B" below; recommend prototyping the unified frame and
  retrofitting the instructor path onto it if it lands cleanly.

---

## Protocol changes (`src/protocol.h`)

Two options. **Recommend Option B** (the unified addressed-text frame) because it
collapses ControlCmd + text-MSG onto one encode/decode/dedup/retransmit path, but
Option A is the smaller, lower-risk step if we want text mode landed without
touching the working instructor path.

### Option A — standalone `MAGIC_TEXT` frame (smaller, additive)

New constants + struct + encode/decode mirroring the existing packet helpers:

```c
constexpr uint8_t MAGIC_TEXT    = 0x54;   // 'T' — canned clue as plaintext
constexpr size_t  TEXT_HDR_LEN  = 4;      // magic, station_id, seq, flags
constexpr size_t  TEXT_MSG_MAX  = 96;     // == config::FOX_MSG_MAX

struct __attribute__((packed)) TextMsg {
    uint8_t station_id;                    // the fox sending the clue
    uint8_t seq;                           // wraps; dedup across the repeat burst
    uint8_t flags;                         // reserved (0); unknown bits ignored
    char    text[TEXT_MSG_MAX + 1];        // NUL-terminated ASCII clue
};

inline size_t encode_text(uint8_t station_id, uint8_t seq, uint8_t flags,
                          const char* text, uint8_t* buf);   // -> TEXT_HDR_LEN + len
inline bool   decode_text(const uint8_t* buf, size_t len, TextMsg& m);
```

Largest frame = 4 + 96 = 100 B, within the 128 B RX buffers already used in
`loop_hunter`/`loop_fox`. `flags` is reserved for future use (e.g. an "alert"
or "verbatim text, do not render Morse" bit).

### Option B — unified addressed-text frame (recommended convergence)

Generalize `ControlCmd` into one frame that carries a `kind` discriminator. The
wire shape is already almost identical (`src_id`, `target_id`, `seq`, NUL-term
ASCII payload — `protocol.h:203`):

```c
constexpr uint8_t MAGIC_FRAME = 0x46;     // 'F' — addressed ASCII payload
enum FrameKind : uint8_t {
    FRAME_CMD   = 0,   // run payload through handle_setup_command() (today's CTRL)
    FRAME_MSG   = 1,   // payload is a Morse clue: render/transmit locally
    FRAME_ALERT = 2,   // payload is sticky-alert banner text (field note §6)
};
// header: magic, kind, src_id, target_id, seq  (5 bytes) + NUL-term payload
```

One `encode_frame`/`decode_frame` pair, one dedup-by-seq, one retransmit-burst
helper, one ACK path — instead of parallel implementations for control vs.
message vs. broadcast. **Caution (from the field note): don't over-merge.** Keep
`MAGIC_EDGE` separate. If Option B is taken, migrate `MAGIC_CTRL`→`FRAME_CMD`
and `MAGIC_BCAST`→`FRAME_ALERT` as a follow-up, leaving compatibility shims for
one release so older nodes still parse the old magics.

**Recommendation:** ship **Option A** first (isolated, testable, zero risk to the
proven instructor path), then evaluate Option B as a refactor once text mode is
hardware-validated. The rest of this plan is written against Option A; the
call-site changes are nearly identical for B.

---

## Config changes (`src/config.h`, `src/config.cpp`)

Add a fox message-delivery mode, persisted in NVS like `keymode`:

- `uint8_t msgmode();` / `void set_msgmode(uint8_t)` — `0 = keyed` (current
  edge/compat keying, default) / `1 = text` (send `TextMsg` frame).
- Restore into a `g_msgmode` global at boot in `main.cpp` (mirror the existing
  `g_keymode = config::keymode()` line at `main.cpp:1237`).
- New console verb `msgmode keyed|text` in `handle_setup_command` (mirror the
  existing `keymode` verb) — writes through to config and updates `g_msgmode`
  live; surface it in `show` output next to `keymode`.

`FOX_MSG_MAX` (96) already bounds the clue and matches `TEXT_MSG_MAX`. No new
storage for the message itself — `config::fox_message()` is the source.

---

## Fox TX path (`src/main.cpp`)

New helper alongside `tx_edge`/`tx_keystate`/`tx_ident`:

```c
// Transmit the canned clue as a repeated TextMsg burst (loss tolerance, like the
// ACK/broadcast bursts). One seq per message cycle; hunter dedups by seq.
static void tx_text(uint32_t now);
```

- New constants near `ACK_REPEATS`: `TEXT_REPEATS` (e.g. 4) and `TEXT_GAP_MS`
  (e.g. 50), reusing the burst idiom from `control_rx_try`/`loop_instructor`.
- Maintain a `static uint8_t text_seq;` incremented once per message cycle.
- In `loop_fox`, branch the message cycle on `g_msgmode`:
  - `g_msgmode == text`: at each cycle top, `send_ident(now)` (still announce
    wpm/char_wpm so the hunter renders at the fox's timing), then `tx_text(now)`
    sends the `TEXT_REPEATS`-copy burst, then `pause_until = now + REPEAT_PAUSE`.
    The keyer/player is **not** run in this mode — no edges go out.
  - `g_msgmode == keyed` (default): the existing keyer/`tx_edge`/`tx_keystate`
    path, unchanged.
- Gate `tx_text` on `!g_tx_halted` and `!rx_window` exactly like the existing
  TX calls, so the instructor RX window and the §6 fox-halt both suppress it.
- The fox still runs **silent** (`set_tone(false)`) — text mode changes only the
  wire format, not the no-beep-fox rule.

Note `MODE_LIVEKEY` is unaffected — `loop_livekey` always keys edges; `msgmode`
applies to the canned `loop_fox` cycle only.

## Hunter RX path (`src/main.cpp`, `loop_hunter`)

Add a `decode_text` branch to the existing RX cascade in `loop_hunter` (after the
`decode_edge` branch, before the fallthrough), gated by `fox_lock` like the
others:

```c
} else if (proto::decode_text(buf, n, tm) &&
           fox_lock(tm.station_id, locked_fox, last_fox_rx, now)) {
    // Canned clue as text: dedup by seq, render Morse locally from config/Ident
    // timing for audio, and show the decoded text directly (no '?' holes).
    ...
}
```

On a fresh seq (dedup `static int last_text_seq`):
- Update presence/lock bookkeeping (`last_signal`, `last_rx`, `rssi`,
  `display::activity()`) like the other branches.
- Drive the **local Morse player** to render audio from `tm.text` at the current
  `rx_wpm`/`rx_char_wpm` (seeded by config, retuned by the fox's `Ident`). This
  is the existing `morse::Player` (`src/morse.h`) — reuse it; the hunter does not
  normally run a Player today, so add a hunter-side player instance (or reuse the
  global `player`) keyed to drive `set_tone`. The hunter is no longer muted-fox:
  it *does* sound the clue, same as it sounds decoded edges today.
- Push the decoded text straight to the display copy line — no decoder needed,
  the text is already plaintext. Show it via the existing `display::hunter`
  text path; no `?` substitution because there are no edges to lose.

A duplicate seq is silently dropped (the burst's redundant copies).

**Rendering note.** Today the hunter hears Morse because it reconstructs key
state from received edges and feeds `set_tone`. In text mode the hunter must
generate the key state itself from the text via `morse::Player` and feed the same
`set_tone`. This is the one genuinely new RX behaviour; everything else is a
decode-cascade addition.

## Instructor (optional, Option B only)

If Option B is taken, `relay <id> msg <text>` could push a clue to a fox the same
way `relay` pushes a command today — but that is a convenience, not required for
field note §7. Out of scope for the first cut.

---

## Files touched

| File | Change |
|------|--------|
| `src/protocol.h` | New `MAGIC_TEXT` + `TextMsg` struct + `encode_text`/`decode_text` (Option A); or unified `MAGIC_FRAME`/`FrameKind`/`encode_frame`/`decode_frame` (Option B). |
| `src/config.h` / `src/config.cpp` | `msgmode()`/`set_msgmode()` persisted in NVS; default `keyed`. |
| `src/main.cpp` | `g_msgmode` global + boot restore; `msgmode` console verb + `show` line; `tx_text()` + `TEXT_REPEATS`/`TEXT_GAP_MS`; `loop_fox` message-cycle branch on `g_msgmode`; `loop_hunter` `decode_text` RX branch + local `morse::Player` render-to-audio. |
| `src/morse.h` / `src/morse.cpp` | No API change expected — reuse `morse::Player` on the hunter side. |
| `docs/protocol.md` | Document `MAGIC_TEXT`/the unified frame + the `msgmode` parameter. |
| `docs/commands.md` | Document the `msgmode keyed|text` console command. |
| `FIELD-NOTES-20260619.md` / `TODO.md` | Mark §7 in progress when implementation starts. |

## Classes / symbols

- `proto::TextMsg` (new) — wire struct, mirrors `BroadcastMsg`/`ControlCmd` shape.
- `morse::Player` (existing, `src/morse.h:27`) — reused hunter-side to render the
  received text to audible Morse at `rx_wpm`/`rx_char_wpm`.
- `morse::Decoder` (existing) — **not used** in the text RX path (no edges to
  decode); it stays in service for the edge/compat path.
- `config::msgmode`/`set_msgmode` (new) — NVS-backed delivery mode, parallels
  `config::keymode`.

## Test plan

- Bench: two stations, fox `msgmode text`, confirm hunter shows the full clue
  text with zero `?` and sounds clean Morse at the announced wpm. Compare against
  `msgmode keyed` at the same attenuation.
- Edge-of-range: with heavy attenuation (or distance), confirm the text frame's
  retransmit burst delivers the whole clue where the edge stream produced
  `????` (the field-note §5 failure this addresses).
- Forward-compat: a hunter on old firmware (no `MAGIC_TEXT`) ignores the frame
  and shows nothing — confirm it does not misparse or crash.
- Mixed: fox alternating `keyed`/`text` across reboots; hunter follows each.

## Sequencing

Land **after** field-note §4 (always-MAX ACK) and §6 (sticky alert + fox halt) —
both are smaller and fix confirmed every-time field problems. Start with Option A
(isolated, testable), hardware-validate, then evaluate the Option B convergence
as a follow-up refactor.
