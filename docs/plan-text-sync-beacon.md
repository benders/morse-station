# Plan — text-mode sync/DF beacon + per-element reveal (field note §8)

Status: **planned, not started.** Follow-on to `plan-text-message-mode.md` (field
note §7, now landed and HW-validated). Text mode fixed *received-message clarity*
(the clue arrives as ground-truth ASCII, no `?` holes) but broke three things in
the field that edge keying gave for free:

1. **Hunters are unsynchronized.** Each hunter renders the clue from a `morse::Player`
   started off its *own* `millis()` whenever its copy of the burst arrived
   (`loop_hunter`, `main.cpp:2301`). Several hunters in earshot beep at different
   times → the sidetones smear together and the Morse is unreadable by ear.
2. **RSSI tracking has nothing to track.** Hunters DF the fox by RSSI, but text
   mode puts ~150 ms of traffic on the air then idles ~12 s (`REPEAT_PAUSE`). The
   bar is dead almost the whole time. DF needs a signal at **≤250 ms** cadence.
3. **Display reveals per character, not per element.** `reveal_to()` pushes a
   whole character's dit/dah pattern in one `ditdah_push(p)` (`main.cpp:2387`), so
   the learner can't watch each `.`/`-` light up in lock-step with each beep.

## Root cause

All three trace to one property of text mode: **the audio is generated locally and
independently on each hunter, and the air goes idle between bursts.** That is
exactly what fixed clarity, but it discards the *shared, continuously-present
signal* that every hunter previously decoded from the same packets at the same
time.

Two orthogonal levers fix it without giving up clean audio:

- **A periodic sync/DF beacon** from the fox gives every hunter (a) a common time
  base to slave its local render to — so all hunters beep together — and (b) a
  carrier on the air at a fixed ≤250 ms cadence to DF. One packet type, both jobs.
- **A per-element reveal** in the hunter render loop is a small, local display
  change, independent of the protocol.

We keep the `TextMsg` frame exactly as-is — it is still the robust, no-`?`
delivery of the clue text and the source of clean local audio. The beacon only
*aligns* the local renders and *fills the air*; it never carries the clue text.

---

## Design overview

The fox runs a **master render clock** for the clue (it already advances a
`morse::Player` in the keyed path; in text mode it advances one purely to source a
position — it stays silent on the air). It emits a `Sync` beacon every
`BEACON_MS` (≈200 ms) for the **entire** fox loop — both while a clue is
"rendering" and during the `REPEAT_PAUSE` between clues — so the DF signal never
dies. The beacon carries:

- the **clue seq** (matches the `TextMsg.seq` of the clue currently being
  rendered), so a hunter knows *which* clue the position refers to and can detect
  a new message;
- the fox's **wpm/char_wpm**, so a hunter that missed the `Ident` can still render;
- the **render position** `pos_ms` — elapsed ms into the clue render — valid only
  while rendering; a sentinel (`POS_IDLE = 0xFFFF`) during the inter-message pause.

Each hunter **free-runs** its local `Player` between beacons (smooth audio) and
**resyncs** to `pos_ms` only when its local position drifts past a slack window.
A missed beacon costs nothing — the next one re-aligns. A station tuning in
mid-message acquires the text from the next `TextMsg` re-send, then **seeks** its
player to the live `pos_ms` and joins the in-progress render already in sync with
everyone else.

```
fox loop (msgmode text):
  cycle top: send Ident; send TextMsg burst; start master render clock (pos=0)
  rendering: every BEACON_MS  -> Sync(seq, wpm, pos_ms)         [DF + slave]
             every TEXT_RESEND_MS -> TextMsg burst (same seq)   [mid-joiners]
  render done: pause REPEAT_PAUSE, still emitting Sync(pos=IDLE) [DF stays alive]
  repeat with seq+1
```

---

## Protocol changes (`src/protocol.h`)

New beacon, mirroring the existing magic-gated helpers (`'S'` = 0x53 is unused):

```c
constexpr uint8_t MAGIC_SYNC   = 0x53;   // 'S' — text-render sync / DF beacon
constexpr uint16_t POS_IDLE    = 0xFFFF; // pos_ms sentinel: no clue rendering now
constexpr size_t  SYNC_LEN     = 7;      // magic, station_id, seq, wpm, char_wpm, pos_ms(u16)

struct __attribute__((packed)) Sync {
    uint8_t  magic;        // MAGIC_SYNC
    uint8_t  station_id;   // the fox
    uint8_t  seq;          // == TextMsg.seq of the clue being rendered
    uint8_t  wpm;          // overall speed (so a mid-joiner can render w/o Ident)
    uint8_t  char_wpm;     // Farnsworth char speed
    uint16_t pos_ms;       // elapsed ms into the render, or POS_IDLE between clues
};

inline size_t encode_sync(uint8_t station_id, uint8_t seq, uint8_t wpm,
                          uint8_t char_wpm, uint16_t pos_ms, uint8_t* buf);
inline bool   decode_sync(const uint8_t* buf, size_t len, Sync& s);
```

7 bytes, magic-gated → forward-compatible (a node predating `MAGIC_SYNC` ignores
it; mixed fleets keep working, exactly as `MAGIC_TEXT` did). `pos_ms` is a u16 ms
counter; a clue longer than 65 s would wrap — clamp/saturate at `POS_IDLE-1` (no
real clue is that long; document the bound).

---

## Config / constants (`src/main.cpp`)

No new NVS config. New constants near `TEXT_REPEATS` (`main.cpp:78`):

```c
static constexpr uint32_t BEACON_MS      = 200;   // sync/DF beacon cadence (<=250 req)
static constexpr uint32_t TEXT_RESEND_MS = 2000;  // re-burst TextMsg for mid-joiners
```

`BEACON_MS` is the answer to problem #2: 5 packets/s, ~12 ms airtime each ≈ 6 %
duty — far below the 25–33 pkt/s that saturated the removed `compat` stream, and
each beacon is independently disposable (no burst needed; the next one is 200 ms
away).

---

## Fox TX path (`src/main.cpp`, `loop_fox` text branch ≈ `main.cpp:1925`)

Today the text branch sends `Ident` + `tx_text()` once, then `pause_until = now +
REPEAT_PAUSE` and idles. Replace the "instant burst then idle" with a clock-driven
render window:

- New helper `tx_sync(now, seq, pos_ms)` alongside `tx_text`/`send_ident`, sending
  one `Sync` packet (single send, **not** a burst).
- Maintain a fox-side master clock: `render_start_ms`, the active `clue_seq`, and a
  `bool rendering`. Advance a local `morse::Player` (silent — never drives the
  radio or sidetone) started on `config::fox_message()` so `pos_ms = now -
  render_start_ms` and `rendering` ends when that player `finished()`. (Using the
  Player's own timeline as the clock keeps fox and hunter render lengths identical.)
- Schedule, each `loop_fox` pass (all gated on `!g_tx_halted && !rx_window`, like
  the existing TX calls so the instructor window and §6 fox-halt still suppress):
  - **cycle top** (new clue): `send_ident(now)`; `tx_text(now)`; start the master
    render clock; record `clue_seq` = the seq `tx_text` just used (expose it —
    `tx_text`'s `text_seq` becomes a field the loop can read, or `tx_text` returns
    it).
  - **every `BEACON_MS`**: `tx_sync(now, clue_seq, rendering ? pos_ms : POS_IDLE)`.
  - **every `TEXT_RESEND_MS` while rendering**: re-`tx_text()` **with the same
    seq** (so a mid-joiner picks up the clue; existing hunters dedup it). This needs
    `tx_text` to accept/keep a fixed seq for the cycle rather than `seq++` per call.
  - **render done**: set `rendering=false`, `pause_until = now + REPEAT_PAUSE`;
    keep emitting `Sync(pos=POS_IDLE)` every `BEACON_MS` through the pause.
- The fox stays **silent on the air** for audio — `set_tone(false)` unchanged. The
  beacon is data, not a keyed carrier; the no-beep-fox rule is intact.

`MODE_LIVEKEY` and the keyed (`msgmode keyed`) path are untouched — they already
put a real keyed signal on the air, so they have sync + RSSI intrinsically and emit
no `Sync` beacons.

---

## Hunter RX path (`src/main.cpp`, `loop_hunter`)

Add a `decode_sync` branch to the RX cascade (after `decode_text`, gated by
`fox_lock` like the others). State additions next to the text-render block
(`main.cpp:2130`):

```c
static int      want_text_seq  = -1;   // beacon seen for a clue we lack text for
static uint16_t last_beacon_pos = proto::POS_IDLE;
static uint32_t last_beacon_ms  = 0;
```

### On a `Sync` beacon (problem #2 + #1)

- Refresh presence/RSSI exactly like the other branches (`last_signal=now`,
  `rssi=r`, `rssi_valid=true`, `display::activity()`, `rx_station_id`). **This is
  what keeps the DF bar alive** — a beacon lands every ≤250 ms across the whole fox
  loop, including the inter-message pause.
- Adopt the beacon's `wpm/char_wpm` if changed (same retune as the `Ident` branch),
  so a mid-joiner that missed the `Ident` still has timing.
- If `s.pos_ms == POS_IDLE`: the fox is between clues — just presence/DF, no render
  action.
- Else (a clue is rendering, `s.seq` is its seq):
  - **We have this clue's text already** (`text_playing && s.seq == last_text_seq`):
    **resync** — if `|player.position_ms() - s.pos_ms| > RESYNC_SLACK_MS`, seek the
    player to `s.pos_ms`; otherwise leave it free-running (see "Missed beacons").
  - **We don't have the text yet** (mid-join, or first beacon of a new clue before
    its `TextMsg` arrived): record `want_text_seq = s.seq` and `last_beacon_pos =
    s.pos_ms`; do **not** render yet (nothing to render). The clue text arrives on
    the next `TextMsg` re-send (≤ `TEXT_RESEND_MS`).

`RESYNC_SLACK_MS` ≈ one dit (`morse::unit_ms(rx_wpm)`, ~90 ms at 13 wpm): smaller
than that, ignore the beacon and free-run (no audible stutter); larger, hard-seek.

### On a `TextMsg` frame (extends the existing branch, `main.cpp:2251`)

Mostly unchanged, but tie in the mid-join handshake:

- Existing dedup-by-seq + "render only when free or changed" logic stays.
- **Mid-join completion:** if a beacon already announced this seq (`want_text_seq ==
  tm.seq`) and we now have the text, start the render **and immediately seek** to
  the last known `pos_ms` (`last_beacon_pos`) so we join the in-progress render in
  lock-step with the other hunters — instead of starting at pos 0 (which would beep
  the start of the clue while everyone else is mid-word). Clear `want_text_seq`.
- A fresh seq with no prior beacon (the normal cycle-top case) starts at pos 0 as
  today; the first beacon ~200 ms later confirms alignment.

### Player drive + missed beacons (problem #1 robustness)

The render loop (`main.cpp:2359`) keeps driving `text_player.update(millis())` every
pass, so **between beacons the player free-runs on the local clock** — audio is
continuous and smooth regardless of beacon loss. The beacon only *corrects* drift:

- **One/few beacons missed:** no effect — the player keeps rendering; the next
  beacon received re-aligns within `RESYNC_SLACK_MS`. Hunters never stall waiting on
  a beacon.
- **Many beacons missed (signal fade):** the player **renders the clue to
  completion anyway** — the hunter has the full text + timing, so a faded-out hunter
  still finishes the current clue cleanly (it just may drift slightly from peers
  until a beacon returns). Presence/RSSI handling is unchanged: the existing
  `signal_timeout_ms` (3 s) clears the RECV id / bar if beacons stop entirely.
- **Backwards jump / seq change** in `pos_ms` (new message started) is handled by
  the seq-change path (treated as a new clue → wipe + restart at the new pos), so a
  stale beacon can never rewind a live render mid-word.

This makes the beacon **advisory, not load-bearing**: present, it tightens all
hunters onto the fox's clock; absent, each hunter degrades to today's
free-running-but-correct local render.

### Per-element reveal (problem #3)

Today `reveal_to()` (`main.cpp:2375`) emits a whole character's pattern at once when
its key-down elements have all sounded. Change it to emit **one symbol per
key-down element** so `.`/`-` appears in step with each beep:

- The render already counts key-down edges in `text_elems` (`main.cpp:2366`) — that
  is already per-element granularity; only the *push* is per-character.
- Track an intra-character element cursor: for the current `rendering_text` char
  with pattern `p` (`morse::pattern(c)`), when the cumulative `text_elems` reaches
  `text_reveal_need + k`, push **`p[k-1]`** (a single `.` or `-`) to `ditdah_buf`.
  When `k == strlen(p)` the character is complete → push the decoded letter to
  `text_buf` and advance `text_reveal_pos` (the text line stays per-character, which
  is correct — a letter is only "known" once fully sent; the *dit/dah* line is what
  becomes per-element).
- Word space (`' '`) still pushes `"/ "` as soon as the preceding char completes
  (carries no elements), unchanged.
- The final-flush path (`reveal_to(0xFFFF)` at `text_player.finished()`) releases
  any trailing partial element/char/space, unchanged in intent.

Net effect: the dit/dah line scrolls one element at a time, each appearing exactly
as its beep sounds — the visual/audio dit-vs-dah training the field note asked for.

---

## `morse::Player` API change (`src/morse.h` / `src/morse.cpp`)

The Player must support **reading and seeking** its position for beacon slaving:

```c
uint32_t position_ms() const;          // elapsed ms into the current render
void     resync(uint32_t now, uint32_t pos_ms);  // align so position==pos_ms at now
```

`resync` recomputes the active segment index and `seg_start_` from an absolute
elapsed `pos_ms` (walk the cumulative `segs_` durations) rather than the current
incremental `seg_start_` model — a small internal refactor to track an absolute
`started_at_`/elapsed base. `position_ms()` is the cumulative offset of `idx_` plus
`now - seg_start_`. Keep `update()/down()/finished()` behaviour identical when no
`resync` is called, so the keyed fox path and existing tests are unaffected.

---

## Edge cases

| Situation | Handling |
|---|---|
| Beacon lost (1–few) | Player free-runs on local clock; next beacon re-aligns. No stall. |
| Beacon stream lost entirely | Render finishes from local text+timing; `signal_timeout_ms` clears the bar after 3 s. |
| Station tunes in mid-message | Beacon gives seq+pos+timing; next `TextMsg` re-send (≤2 s) gives text; player seeks to live `pos_ms` → joins in sync. |
| Beacon for seq we have no text for | `want_text_seq` set; render deferred until matching `TextMsg`; then seek to last `pos_ms`. |
| `pos_ms` jumps backward / seq increments | New clue → wipe copy buffers, restart render at new pos (existing seq-change path). |
| Drift < one dit between beacons | Ignored (free-run) to avoid audible stutter; only `> RESYNC_SLACK_MS` hard-seeks. |
| Clue render > 65 s | `pos_ms` clamped at `POS_IDLE-1`; documented bound (no real clue approaches this). |
| Old firmware hunter | Ignores `MAGIC_SYNC` for free; behaves as today (unsynced, bursty RSSI) — graceful. |
| Inter-message pause | Beacons continue with `pos=POS_IDLE` → DF bar stays alive, no render action. |

---

## Files touched

| File | Change |
|------|--------|
| `src/protocol.h` | `MAGIC_SYNC`/`POS_IDLE`/`Sync` struct + `encode_sync`/`decode_sync`. |
| `src/morse.h` / `src/morse.cpp` | `Player::position_ms()` + `Player::resync()`; absolute-position refactor. |
| `src/main.cpp` | `BEACON_MS`/`TEXT_RESEND_MS`; fox master render clock + `tx_sync()` + scheduled beacon/text-resend in `loop_fox` text branch; `tx_text` fixed-seq-per-cycle; hunter `decode_sync` branch + slave/seek/mid-join + `want_text_seq`; per-element `reveal_to`. |
| `docs/protocol.md` | Document `MAGIC_SYNC`/`Sync`, the beacon cadence, and the master-clock/slave model under "Model — text-frame canned message". |
| `docs/commands.md` | No new command (BEACON_MS is fixed); note beacon behaviour if `debug` dump gains an `RX S` line. |
| `FIELD-NOTES-20260619.md` / `TODO.md` | Mark §8 in progress. |

---

## Test plan

- **Sync (bench, 2+ hunters):** one fox `msgmode text`, two hunters side by side.
  Confirm both beep the clue *together* (audibly in unison), not smeared. Pull one
  hunter's antenna briefly mid-clue → it free-runs, then snaps back into unison on
  the next beacon.
- **RSSI continuity:** watch a hunter's RSSI bar across a full fox cycle — it should
  refresh every ≤250 ms through both the render and the `REPEAT_PAUSE`, never
  dropping to "RECV ---" while the fox is present.
- **Mid-join:** power a hunter on *during* a clue render. It should stay blank for
  ≤ `TEXT_RESEND_MS`, then begin sounding the clue **from the current position** (mid-
  word), in sync with a hunter that heard the whole clue.
- **Per-element:** with `showtext` on the dit/dah view, confirm each `.`/`-`
  appears as its beep sounds, not a whole letter at once.
- **Beacon-loss tolerance:** heavy attenuation so most beacons drop — confirm the
  clue still renders to completion (audio + full text) on each hunter, just less
  tightly synced.
- **Forward-compat:** a hunter on §7 firmware (no `MAGIC_SYNC`) still renders the
  clue from `TextMsg` alone (unsynced, bursty RSSI) — confirm no misparse/crash.

## Sequencing

Land after §7 is confirmed stable in the field. Suggested commit order (each builds
+ HW-checks before the next):

1. `morse::Player::position_ms()`/`resync()` + unit check (keyed path unaffected).
2. `MAGIC_SYNC`/`Sync` encode/decode in `protocol.h`.
3. Fox master clock + `tx_sync` + scheduled beacons/text-resends (verify on air with
   `debug`, no hunter changes yet).
4. Hunter `decode_sync` slave + seek + mid-join.
5. Per-element `reveal_to` (independent; can land any time).
