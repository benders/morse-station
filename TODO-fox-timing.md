# TODO: Fox-side TX timing jitter

Status: **open / investigation notes** (not a regression — pre-existing,
surfaced during the `?`/poison bench validation, 2026-06-17).

## Symptom

A fox occasionally transmits an element or gap whose duration is *stretched*
well beyond its nominal value, so a receiver decodes a different (but valid)
letter than was sent — with **no packet loss involved**. Example seen on the
bench: the fox sent `S` (`...`) but the receiver decoded `I` (`..`), because the
intra-character gap before the third dit was stretched into a full character
gap. This produces a confident *wrong letter*, and unlike packet loss it cannot
be caught by the sequence-gap `?` machinery (see "Why poison/seq can't fix it").

## Concrete evidence

Hunter 43 decoding fox **73 (M5 Cardputer)**, 18 WPM, edge keymode, from the
capture `/tmp/bench-1781757214.log`. Raw edge dump for the offending character
(`dn` = TX-measured duration of the segment that just ended, ms):

```
RX E 73 seq=74 lvl=0 hb=0 dn=66    -> EL .   (S dit 1)
RX E 73 seq=75 lvl=1 hb=0 dn=198   -> CH E   (closes the prior char)
RX E 73 seq=76 lvl=0 hb=0 dn=66    -> EL .   (S dit 1 ... new char)
RX E 73 seq=77 lvl=1 hb=0 dn=66            (normal intra-char gap)
RX E 73 seq=78 lvl=0 hb=0 dn=66    -> EL .   (S dit 2)
RX E 73 seq=79 lvl=1 hb=0 dn=330   -> CH I   (gap STRETCHED 66 -> 330 ms)
```

- **Sequence numbers are consecutive (74→79): no packet was lost.**
- At 18 WPM one unit ≈ `1200/18` ≈ 66 ms. The intra-character gap should be
  ~66 ms; here it was measured and transmitted as **330 ms** (~5 units), which
  exceeds the char-gap threshold, so the receiver correctly closed the character
  after two dits → `..` = `I`.
- `dp` (dur_prev) on that packet was the normal 66 ms, i.e. the stretch was a
  one-off on that single segment.

The receiver did exactly the right thing for the timing it was handed — the
fault is on the transmit side: the fox really did hold that gap for 330 ms.

## Root cause (hypothesis)

The fox keys and *measures* edge durations from the cooperative Arduino
`loop()`:

- `src/main.cpp:tx_edge()` (~L1315) computes `dur = now - seg_start_ms` from
  `millis()` at the moment the loop notices the key level changed.
- The key level comes from `morse::Player`, advanced once per `loop()` via
  `player.update(now)` in `loop_fox()` (`src/main.cpp:1569`).

So segment-boundary detection happens at *loop cadence*, not on a real-time
clock. If a single `loop()` iteration runs long, the next level transition is
observed late and `dur` overshoots by the stall length. Because the same
`loop()` also drives the actual RF keying, the on-air gap genuinely is stretched
— and the measured/transmitted `dur_now_ms` faithfully reports it.

The Cardputer (stn 73) is the worst offender: `M5Unified` LCD refresh, the
ES8311/NS4150B audio path, and BLE all share the loop and can block it for tens
to hundreds of ms. Heltec V3/V4 foxes have far less per-loop peripheral work and
should jitter much less (unconfirmed — worth measuring). Related known Cardputer
contention notes: amp brownout at boot, boot-loops on the 80 MHz power-saving
build.

## Why poison/seq can't fix it

The `?`-on-ambiguity and poison-character work keys entirely off **EdgeEvent
sequence gaps** (a lost packet ⇒ `seq` jumps ⇒ poison the char ⇒ one `?`). In
this failure there is **no lost packet and no seq gap** — the fox transmitted a
clean, in-order stream that simply contained a wrong duration. The receiver has
no signal that anything went wrong, so it (correctly, by its thresholds) emits a
confident wrong letter. This is the inherent "timing/threshold" class flagged in
the original design discussion, distinct from packet loss.

## Candidate mitigations (rough ranking)

1. **Stabilize the fox keying timebase (preferred).** Sample the `Player` /
   measure edges on a fixed-interval source decoupled from `loop()` jitter —
   e.g. a hardware-timer ISR or a dedicated FreeRTOS task (pinned to the core
   not running the display/audio) that advances the player and emits edges. Then
   a slow display/audio frame can't stretch a measured element/gap. Biggest win,
   most work; needs a per-platform timer seam (ESP32 vs nRF52).
2. **Cut worst-case per-loop latency on the fox**, especially Cardputer: throttle
   / defer LCD redraws, avoid blocking audio/BLE calls while a message is
   playing. Cheaper, partial — narrows the jitter window but doesn't bound it.
3. **TX-side duration sanity snap.** Before sending, quantize a measured segment
   to the nearest expected unit multiple given the configured WPM, rejecting
   implausible values. Risky: hides genuine Farnsworth/odd timing and can paper
   over real problems; only a fallback.
4. **Prefer a Heltec as the fox** for exercises where decode fidelity matters,
   until (1) lands. Operational workaround, not a fix.

Receiver-side heuristics (e.g. "this gap is suspiciously long → `?`") were
considered and rejected: they reintroduce guessing, are fragile across WPM /
Farnsworth settings, and would mask (1).

## Next steps

- [ ] Quantify the jitter: run a fox on each board type (Cardputer, Heltec V3,
      Heltec V4, Wio, RAK) and log the distribution of `dur_now_ms` for known
      elements/gaps at a fixed WPM. Confirm Cardputer is the outlier and get a
      worst-case number per board. (Harness: `scripts/edge_test.py` / the
      `RX E` dump already prints `dn`.)
- [ ] Prototype a timer/task-driven keying path on one ESP32 board and re-measure.
- [ ] Decide whether mitigation (2) alone is "good enough" for classroom use, or
      whether (1) is warranted.

## Code references

- `src/main.cpp:tx_edge()` (~L1315) — edge measurement (`dur = now - seg_start_ms`).
- `src/main.cpp:loop_fox()` (~L1569) — per-loop `player.update(now)` keying.
- `src/morse.cpp` `Player::update()` — segment advance from `millis()`.
- `docs/edge-events.md` "Loss handling" — the seq/poison path this issue is
  *not* covered by.

---

# Implementation plan — mitigation #1 (stabilize the fox keying timebase)

Status: **planned, ready to implement** (added 2026-06-18). Scope: **Fox mode,
edge keymode** (`MODE_FOX` + `KEYMODE_EDGE`), the path the bench capture used and
the forward-going default. Compat keymode and livekey are explicitly out of
scope for this change and stay on the existing in-loop path (see "Scope &
non-goals").

## Core idea

The decode in edge mode is driven **entirely by the transmitted `dur_now_ms`**,
not by packet arrival time (`Decoder::feed_segment(on, dur_ms)` in
`src/morse.cpp`). So we do **not** need to make the radio transmit on a stable
clock — we only need to *measure* each element/gap duration on a stable clock.
If durations are measured on a fixed-cadence timebase decoupled from `loop()`,
a slow display/audio/BLE frame can no longer stretch a measured segment, and the
actual `radio::send()` is free to jitter without corrupting the decode.

Therefore: **move player advance + edge detection + duration measurement off
`loop()` and onto a fixed-interval keyer**, and leave packetization + radio TX in
`loop()`, fed by a queue.

The keyer is **gated to active message playback** — started when a message
begins, stopped the instant it finishes — so it ticks only during the few
seconds of keying and is completely quiet through the `REPEAT_PAUSE`. See
"Power-saving interaction" below; this is a hard requirement, not an
optimization.

## Architecture

Producer/consumer split, single-producer/single-consumer (SPSC):

```
  ┌─────────────────────────── keyer (fixed 2 ms cadence) ───────────────────┐
  │  platform timer/task → keyer_tick():                                      │
  │    now = keyer monotonic ms                                               │
  │    [consume start/stop command from loop, under mutex]                    │
  │    if !player.finished(): player.update(now)                              │
  │    detect player.down() transition vs last_level                          │
  │      on edge:      push {EDGE,  level_entered, dur_now}  to ring          │
  │      on finish:    set finished flag (loop owns pause/restart)            │
  │    else if steady ≥ HEARTBEAT_MS: push {HEARTBEAT, level, elapsed}        │
  └───────────────────────────────────────────────────────────────────────────┘
                                   │  SPSC ring buffer (lock-free, atomics)
                                   ▼
  ┌─────────────────────────────── loop_fox() ───────────────────────────────┐
  │  drain ring → build proto::EdgeEvent (maintain prev_dur) → radio::send()  │
  │    (suppressed during rx_window / g_tx_halted, as today)                  │
  │  own pause/restart: when keyer reports finished → keyer_stop() (sleepable │
  │    pause); when now ≥ pause_until, send_ident(), keyer_start()+start cmd   │
  │  unchanged: prg tap, instructor rx_window, ident, halt                    │
  └───────────────────────────────────────────────────────────────────────────┘
```

Why this split:
- All timing-sensitive work (advance + measure) runs at a stable 2 ms cadence.
  At 18 WPM one unit ≈ 66 ms, so edge quantization is ≤ ±2 ms — negligible vs.
  the 100+ ms loop stalls causing the bug, and honest (no snapping/quantizing to
  nominal, so genuine Farnsworth/odd timing is preserved — this is *not*
  mitigation #3).
- `radio::send()` (SPI, blocking) stays in `loop()`; it must never run from the
  keyer context. Its jitter no longer matters because the duration is already
  captured.
- The Player keeps its existing self-correcting accumulator (`seg_start_ +=
  segs_[idx_].ms`), so even multi-segment catch-up after a keyer hiccup stays
  phase-correct.

## Platform seam (new)

Add to `src/platform.h` (mirrors the existing `watchdog_*` seam style):

```cpp
// Fixed-cadence keyer tick, decoupled from loop() jitter. Invokes `cb` every
// period_us microseconds from a high-priority context independent of the
// Arduino loopTask: an esp_timer periodic callback on ESP32, a FreeRTOS task
// (vTaskDelayUntil) above loop priority on nRF52. `cb` MUST be short and
// non-blocking — it samples the Morse player and timestamps key edges only:
// NO radio SPI, NO Serial, NO flash. Returns false if the timer/task could not
// be created (caller falls back to in-loop sampling — never worse than today).
bool keyer_start(void (*cb)(), uint32_t period_us);
void keyer_stop();
// Monotonic microsecond clock used by the keyer for duration measurement.
uint32_t keyer_now_us();
```

- `src/platform_esp32.cpp`: `esp_timer_create` once; `keyer_start`/`keyer_stop`
  map to `esp_timer_start_periodic(period_us)` / `esp_timer_stop` so re-arming
  per message is cheap and the timer is genuinely stopped (no wake-ups) during
  the pause. esp_timer runs the callback in its own high-priority task — it is
  clock-agnostic, so it does **not** retrigger the Cardputer M5/APB boot-loop
  hazard (that was `setCpuFrequencyMhz`, see `set_cpu_low_power`).
  `keyer_now_us()` → `esp_timer_get_time()` (µs since boot, truncated to 32-bit).
- `src/platform_nrf52.cpp`: `xTaskCreate` a small task **once** (priority
  `TASK_PRIO_HIGH` — above the Adafruit loopTask, below the SoftDevice); the task
  body `vTaskDelayUntil`s at `pdMS_TO_TICKS(period_us/1000)` (2 ms ≥ 1 tick at
  1024 Hz). `keyer_start`/`keyer_stop` `vTaskResume`/`vTaskSuspend` it so a
  suspended keyer adds no ticks and lets FreeRTOS tickless idle / the SoftDevice
  reach low-power wait through the pause. `keyer_now_us()` → `micros()`.
- Heltec V3/V4 + RAK get the ESP32/nRF52 impls for free (compile-only here).

## Concurrency model

- **Edge ring buffer** (`src/main.cpp`): fixed array of
  `struct EdgeRec { uint8_t flags; uint16_t dur_ms; }` (≈32 entries). `std::atomic`
  head (consumer=loop) / tail (producer=keyer). Lock-free SPSC — no mutex on the
  hot path. On overflow: drop + bump a `g_keyer_drops` counter surfaced in
  `show`/debug (should stay 0; non-zero ⇒ loop not draining fast enough).
- **Start/stop command handoff** (loop → keyer): a FreeRTOS mutex
  (`xSemaphoreCreateMutex`, same API on both cores) guarding a small struct
  `{ bool start_req; bool run; char text[MSG_MAX]; }`. Taken only at message
  boundaries (≈ every REPEAT cycle) and for ≤ a few µs in the keyer, so contention
  is nil. The keyer owns `player`; `loop()` never calls `player.update/start/down`
  in the fox edge path anymore — it only reads an atomic `g_keyer_finished` flag.

## Files & symbols to modify

1. `src/platform.h` — declare `keyer_start/keyer_stop/keyer_now_us` (doc comment).
2. `src/platform_esp32.cpp` — esp_timer implementation + `keyer_now_us`.
3. `src/platform_nrf52.cpp` — FreeRTOS task implementation + `keyer_now_us`.
4. `src/main.cpp`:
   - add `EdgeRec` SPSC ring (atomas) + `KeyerCmd` struct + mutex + `g_keyer_*`
     state (`g_keyer_finished`, `g_keyer_drops`).
   - new free fn `keyer_tick()` (the registered `cb`): owns `player.update`, edge
     detection, heartbeat synthesis, enqueue. Reuses the segment/level/prev-dur
     bookkeeping currently inside `tx_edge` — that state machine moves here.
   - new `fox_keyer_drain(now)` in/near `loop_fox`: pop `EdgeRec`s, build
     `proto::EdgeEvent` (carry `dur_prev_ms` across pops), `radio::send` (still
     gated by `rx_window`/`g_tx_halted`).
   - `loop_fox()`: replace the `player.update` + `tx_edge` block with
     "issue start command at top-of-loop / drain queue"; keep prg tap, rx_window,
     ident, halt logic.
   - `loop_fox()` keyer lifecycle (power-gated): the keyer is **not** started in
     `setup()`. In the fox edge path, `keyer_start(keyer_tick, 2000)` is called at
     each message *start* (alongside the start command) and `keyer_stop()` the
     instant the keyer reports `finished`, so it is idle for the whole
     `REPEAT_PAUSE`. `keyer_start` must be cheap to re-arm (esp_timer
     start/stop_periodic; FreeRTOS task created once then suspended/resumed). If
     `keyer_start` ever returns false, set `g_keyer_fallback` and use the legacy
     in-loop `tx_edge` path for that message.
   - keep `tx_edge()` compiled as the fallback path (guarded by `g_keyer_fallback`)
     so we are guaranteed never worse than today.
5. `docs/edge-events.md` — add a "Keying timebase" subsection describing the
   keyer task and that durations are measured off-loop.
6. `TODO-fox-timing.md` — mark mitigation #1 done + paste before/after numbers.

`src/morse.{h,cpp}` need **no change** (Player stays as-is; the keyer just drives
`update()`/`down()` from a steadier clock).

## Scope & non-goals

- Only `MODE_FOX` + `KEYMODE_EDGE`. Compat keymode stays on the 30 ms in-loop
  sampler. `MODE_LIVEKEY` stays in-loop (human keying has its own input-sampling
  jitter; a separate effort if wanted).
- We do **not** stabilize radio TX timing (unnecessary — see "Core idea").
- We do **not** snap/quantize durations to nominal (that is mitigation #3, which
  the design rejects for hiding real timing).

## Power-saving interaction (hard requirement)

The fox's largest remaining sleepable window is the ~12 s `REPEAT_PAUSE` between
message repeats; the power roadmap's next fox item is light-sleep through it
(see the power-saving notes — display idle-blank @60 s, 80 MHz on Heltec,
BLE-off AUTO, SX1262 hibernate are already in `main`). A **free-running** 2 ms
keyer would wake the core 500×/s forever — on ESP32 it wakes
`esp_light_sleep_start`, on nRF52 it defeats FreeRTOS tickless idle /
`sd_app_evt_wait` — and would therefore **block sleep-through-pause outright**,
as well as add a small always-on draw today.

Gating the keyer to active playback (start on message start, stop/suspend on
`finished`) removes that conflict: the 2 ms cadence runs only during the few
seconds of keying, when the radio + loop are already busy (negligible
incremental draw), and the entire `REPEAT_PAUSE` stays keyer-quiet and fully
sleepable. The start/stop points coincide with the message-boundary command
handoff, so this is a small addition, not a redesign. The instructor `rx_window`
(pause-tail RX) and the display-blank / BLE-off AUTO policies are all idle-time
behaviours and are unaffected — they never overlap active keying. Hibernate
(SX1262 `SetSleep`) never reaches the run loop, so `keyer_start` is never called
on that path.

Net: with gating, impact on power work is neutral-to-positive (it keeps the
sleep-through-pause door open and tidies the active/idle boundary).

## Test plan

Targets: **Cardputer ADV = stn 73** (ESP32, the worst offender) and **Wio
Tracker L1** (nRF52). Hunter/decoder: a **Heltec (stn 43)**. Harness:
`scripts/edge_test.py` (provisions fox+hunters over serial, `debug on`, captures
`RX E … dn=` + `CH` lines, scores decode) and `scripts/devices.sh` to resolve
ports. Fixed settings for all runs: **18 WPM, plain timing, keymode edge**.

### A. Baseline (BEFORE — current `main` firmware)
1. Flash current `main` to the Cardputer fox and a Heltec hunter (Wio fox run
   done separately with the same hunter).
2. Run `edge_test.py --fox 73 --hunters 43 --duration 600` (10 min), logging to
   `/tmp/fox-timing-baseline-card.log`. Repeat with the Wio as fox →
   `/tmp/fox-timing-baseline-wio.log`.
3. Parse every `RX E … dn=` line; bucket `dn` by expected nominal
   (dit/dah/intra-gap/char-gap/word-gap at 18 WPM, unit≈66 ms). Report per board:
   - histogram + **worst-case `dn`** for segments that should be **one unit**
     (the intra-char gap is the failure locus — nominal ~66 ms);
   - count of "stretched" segments (a 1-unit segment measured ≥ chargap threshold);
   - decode error count / score from `edge_test.py`.
   Expectation: Cardputer shows outliers ≫ 66 ms and ≥1 mis-decode; Wio cleaner.
   (A small ad-hoc parser is fine; the `dn` field is documented in the memory
   "edge debug-dump lvl semantics" — pair `dn` with the segment that *ended*.)

### B. After (NEW firmware)
4. Implement; `pio run` **all 5 envs** must compile (seam parity:
   heltec_v4, heltec_v3, cardputer_adv, rak4631, wio_tracker_l1).
5. Flash new firmware to Cardputer (stn 73) and Wio. Confirm clean **POWERON**
   boot on each (Cardputer especially — watch for the M5/APB boot-loop; esp_timer
   should be safe). Confirm `show` reports `g_keyer_drops = 0`.
6. Re-run the identical `edge_test.py` captures →
   `/tmp/fox-timing-after-card.log`, `/tmp/fox-timing-after-wio.log`.
7. **Stress variant:** during an after-run on the Cardputer, induce loop latency
   (e.g. force LCD redraws / `debug on` chatter / BLE activity) and confirm `dn`
   stays clustered — this is the direct proof the keyer decoupled measurement
   from loop stalls.

### Acceptance criteria
- After-run intra-char-gap `dn` clustered at **66 ± a few ms**, with **no**
  1-unit segment measured at ≥ the char-gap threshold on either board.
- **Zero** timing-attributable mis-decodes over the 10-min run on both boards;
  `edge_test.py` decode score ≥ its pass ratio (parity with baseline-Heltec).
- `g_keyer_drops == 0`; watchdog still fed (no `TASK_WDT`/`WATCHDOG` in
  `bootlog`); all 5 envs build; no Cardputer boot-loop.
- **Keyer idle during `REPEAT_PAUSE`** (power gate): verify no keyer ticks fire
  while `player.finished()` — e.g. a debug tick counter that is flat across the
  pause and only advances during active keying (or a current-meter check on a
  Heltec showing no 2 ms-cadence activity in the pause). This guards the
  sleep-through-pause feature.
- Before/after numbers pasted into this file; mitigation #1 checkbox ticked.

### Risks & fallbacks
- **Concurrency bug → garbled durations.** Mitigate: lock-free SPSC ring with
  atomics + brief mutex only at message boundaries; `g_keyer_drops` guard.
- **Cardputer boot-loop (M5/APB).** esp_timer is clock-agnostic (unlike
  `set_cpu_low_power`); verify clean boot on USB *and* battery.
- **Keyer can't start.** `keyer_start` returns false → `g_keyer_fallback` keeps
  the legacy in-loop `tx_edge` path; never worse than today.
- **nRF52 task priority.** Keep above loopTask, below SoftDevice; keep `cb` tiny
  so it can't starve the radio/BLE stack.
