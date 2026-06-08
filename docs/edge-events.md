# Edge-Event Keying (design + implementation plan)

Replaces the 30 ms **keystate sampling** stream (`docs/protocol.md`, "approach
A") for live/fox keying with an **edge-event** model: the transmitter sends a
packet only when the key *changes*, and that packet carries the *measured
duration of the segment that just ended*. The receiver reconstructs perfect
element timing from those explicit durations instead of sampling a jittered
envelope.

This document is the plan; the firmware is authoritative once landed.

## Why

Sampling key state every 30 ms reproduces a 60 ms dit (20 WPM) with only ~2
samples and ±30 ms (= ±½ dit) edge jitter once loss and scheduling are
included. No amount of decoder tuning beats that floor because it is set by the
*transport*. Edge-events move the timing measurement to the side that owns the
key (the TX), so distance/latency/loop-jitter no longer corrupt the decode.

## Requirements (this work)

1. **Proper behavior on signal loss** — no latched tone, no corrupted decode,
   bounded resync.
2. **Live keying 5–20 WPM** — unit 240 ms (5 WPM) down to 60 ms (20 WPM);
   shortest inter-edge spacing ≈ 60 ms; longest gap (word space at 5 WPM,
   Farnsworth-stretched) ≈ 1.7 s+.
3. **Mixed fleet** — nodes sit at different distances *and run different
   builds*. Any pairing must keep working; far nodes see more loss.

## Packet

New magic so legacy nodes (which only match `MAGIC 0x4D`) cleanly ignore it.

```c
constexpr uint8_t MAGIC_EDGE = 0x45;   // 'E'

struct __attribute__((packed)) EdgeEvent {
    uint8_t  magic;        // MAGIC_EDGE
    uint8_t  station_id;
    uint8_t  seq;          // per-edge, wraps mod 256 (loss/dup/reorder detect)
    uint8_t  flags;        // bit0 = new key level entered (1=down,0=up);
                           // bit1 = heartbeat (no real edge, just a re-assert)
    uint16_t dur_now_ms;   // duration of the segment that JUST ENDED
    uint16_t dur_prev_ms;  // duration of the segment before that (self-heal)
};                         // 8 bytes -> ~25 ms airtime at 4.8 kbps GFSK
```

Levels strictly alternate, so `flags.bit0` + the two durations let a receiver
reconstruct the last two segments completely. **A single lost edge packet is
fully recovered from the next one** (its `dur_prev_ms` restates the segment the
lost packet carried). Only two consecutive losses drop an element — rare, and
bounded by the heartbeat below.

`dur_*` are `uint16` ms (max 65 s) — covers the slowest word gap with room to
spare; resolution is exact ms.

### Idle heartbeat

When the key has been steady longer than `HEARTBEAT_MS` (≈ 700 ms), the TX
re-sends the current state as an EdgeEvent with `flags.bit1=1` and
`dur_now_ms` = elapsed-so-far. Purpose:

- **Presence** — without the 30 ms stream, the hunter needs a liveness beat to
  tell "idle but here" from "gone."
- **Re-anchor** — a late-joining or recently-lost hunter relocks absolute level
  within one heartbeat.
- **Double-loss recovery** — bounds worst-case decode resync to ~1 beat.

A heartbeat is idempotent (absolute level + seq dedup) so it never injects a
spurious element.

## Loss handling (the explicit requirement)

| Failure | Detection | Response |
|---|---|---|
| Single edge lost | `seq` gap == 1 | Next edge's `dur_prev_ms` reconstructs the missing segment → feed both to decoder in order. No visible glitch. |
| Two+ edges lost | `seq` gap >= 2 | Unrecoverable boundary: flush the decoder's pending character (emit nothing or `?`), re-anchor absolute level from the next edge/heartbeat. |
| Key-down then silence (mid-element drop) | no packet for `T_silence` | Force key-up (kill tone so it can't latch), flush pending element, mark segment lost. |
| Prolonged silence | no packet for `signal_timeout_ms` (existing 3 s) | Clear RECV id + RSSI bar (existing behavior). |

`T_silence` is driven by the **heartbeat**, not by CW gap length, so a
legitimate 1.7 s word gap at 5 WPM is never a false drop: set
`T_silence ≈ 2.5 × HEARTBEAT_MS` (~1.8 s) — longer than any single keyed
element/gap between heartbeats, shorter than the 3 s presence timeout.

Far nodes need no per-node tuning: single-loss self-heal + ~700 ms heartbeat
re-anchor absorb their higher loss rate.

## Decode path

Two consumers of each received edge, deliberately split:

- **Sidetone (real-time):** toggle the tone to the packet's absolute level the
  instant it arrives. Latency = one-way radio time (tens of ms) — inaudible.
  The `T_silence` watchdog guarantees it can't latch on.
- **Decoder (timing-accurate):** fed the *explicit durations* from the packet,
  never local arrival timestamps. New entry point on `morse::Decoder`:

  ```c
  // Edge-mode: classify a completed segment from its true (TX-measured) length.
  // `on` = the segment was key-down (element) vs key-up (gap). Emits chars the
  // same way update() does; reuses the wpm/char_wpm thresholds from begin().
  char feed_segment(bool on, uint16_t dur_ms);
  void flush();   // loss/resync: drop any half-built character
  ```

  Thresholds already derive from the Ident-announced `wpm`/`char_wpm`
  (`docs/protocol.md`), so slow/Farnsworth sending decodes correctly. The
  polling `update(key_down, now)` stays for legacy KeyState RX.

## Mixed-fleet / migration

`MAGIC_EDGE` means an **old hunter ignores a new fox** (sees no `0x4D`) — so we
cannot just cut the fox over. Strategy:

1. **Hunters become bilingual first.** A new hunter decodes *both* KeyState and
   EdgeEvent. This is backward compatible — it still hears every existing fox.
   Flash hunters with no fleet-wide coordination.
2. **Foxes flip per-unit once their audience is bilingual.** New
   `keymode` config (`compat` = stream KeyState, default; `edge` = emit
   EdgeEvents) togglable over the air via the existing BLE/serial console
   (parity with `wpm`, `mute`, …). No dual-emit — a fox is one or the other, so
   there's no airtime penalty.

Result: every pairing works at the *lower* of the two capabilities; the fleet
migrates one unit at a time with no flag day. Ident is unchanged and used by
both modes.

## TX timing safety

`radio::send()` blocks (~25 ms for an EdgeEvent) < the 60 ms minimum element at
20 WPM. The edge is **timestamped in the key handler before** the blocking
send, so durations stay exact even though the send straddles into the next
element's start. A burst-retransmit was rejected: 3× ≈ 70 ms would overrun a
60 ms dit and starve key scanning — the `dur_prev_ms` self-heal gives the same
loss protection for one transmission's airtime.

## Unattended test instrumentation

The bench has three boards (different builds → exercises the cross-build
requirement directly): **Wio Tracker L1 Pro = station 115** (nRF52840),
**Heltec V4.3 = station 43** and **Heltec V4.2 = station 42** (both ESP32,
`env:heltec_v4`). They are driven entirely over their USB serial consoles — no
human keying or listening — using the existing tooling:

- `scripts/devices.py` resolves station id → `/dev/cu.usbmodem*` port.
- `scripts/provision.py --port … --id/--wpm/--msg …` writes NVS over the boot
  setup console.
- `scripts/monitor.py --port … --duration N` captures a board's serial without
  a TTY.

Two firmware additions make decode observable without sidetone:

1. **`debug on|off` runtime console command** (serial + BLE), default off. When
   on, a node prints one parseable line per event:

   ```
   RX E <sid> seq=<n> lvl=<0|1> hb=<0|1> dn=<ms> dp=<ms> rssi=<dbm>   # EdgeEvent
   RX K <sid> seq=<n> lvl=<0|1> rssi=<dbm>                            # KeyState (compat)
   RX I <sid> wpm=<n> cwpm=<n> call=<sign>                            # Ident
   EL <sid> <.|->                                                     # element classified
   CH <sid> <char>                                                   # character decoded
   TX E seq=<n> lvl=<0|1> hb=<0|1> dn=<ms> dp=<ms>                    # fox/livekey side
   ```

   The `EL`/`CH` lines are the decoded dits/dahs the user asked for; the `RX *`
   lines are the raw packet dump. Human-readable and grep-friendly.

2. **Mode/keymode are NVS-persisted and auto-resume on boot** (already true for
   mode via `boot_mode`; add the same for `keymode`). So the harness sets a
   board's role + keymode, resets it, and it comes up running that role after
   the menu's 5 s idle auto-select — no interaction.

### Harness — `scripts/edge_test.py`

```
edge_test.py --fox 43 --hunters 115,42 --keymode edge \
             --msg "PARIS PARIS PARIS" --wpm 13 --duration 40
```

Procedure: provision the fox (msg/wpm/keymode/`mode FOX`) and each hunter
(`mode HUNTER`), reset all, send `debug on` to the hunters once they reach the
run loop, capture their serial for `--duration`, parse `CH` lines into decoded
text, and assert the decoded stream contains the sent message (allowing for
mid-loop join). Exit non-zero on mismatch. Runs the matrix unattended:

- **edge fox → both hunters** decode the message (core path).
- **compat fox** (`--keymode compat`) → both hunters still decode (backward
  compat / bilingual RX).
- each board type as fox in turn (Wio-nRF52 ↔ Heltec-ESP32) → cross-build.
- `RX E` dump's `seq` continuity + `dp` self-heal visible for loss inspection
  (true range/loss testing still wants physical distance = user).

## Phased implementation (build + commit each phase before the next)

- **E1 — protocol.** Add `MAGIC_EDGE`, `EdgeEvent`, encode/decode, constants to
  `src/protocol.h`. Compiles unused. Commit.
- **E2 — decoder.** Add `feed_segment()` + `flush()` to `morse::{h,cpp}`;
  keep `update()`. Bench-test classification at 5/13/20 WPM with synthetic
  durations. Commit.
- **E3 — TX edge emit.** Edge tracking w/ timestamps + heartbeat in
  `loop_livekey`/`loop_fox`, gated behind `keymode` (default `compat` →
  no behavior change yet). Commit.
- **E4 — bilingual hunter + debug serial.** Decode `EdgeEvent` in
  `loop_hunter`: real-time sidetone from absolute level, durations →
  `feed_segment`, seq loss detect + single-loss heal, `T_silence` watchdog,
  heartbeat presence. KeyState path untouched. Add the `debug on|off` console
  command + the `RX */EL/CH/TX E` dump above. Commit.
- **E5 — keymode toggle (persisted).** `keymode <compat|edge>` console command,
  persisted in NVS and auto-resumed on boot; flip a fox over the air. Commit.
- **E6 — unattended harness + run.** Write `scripts/edge_test.py` (+ `.sh`
  wrapper) per above; run the full matrix on stations 115/43/42 at 5/13/20 WPM,
  both keymodes, each board as fox. Capture results. Commit.
- **E7 — field tune.** Only after E6 passes on the bench: physical-distance
  loss/decode, then classifier polish ("approach 1") if anything remains.
  Update `docs/protocol.md` with the edge model. Commit.
