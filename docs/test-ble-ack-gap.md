# Test plan — BLE-attached instructor ACK gap (#3)

Validates the multi-ACK fix for the failure recorded on 2026-06-18: with a phone
connected to the **instructor** over BLE, `relay <id> <cmd>` delivered the command
but surfaced **zero ACKs** (0/4), while the same relay over USB serial was 4/4.

## What changed (the thing under test)

- **Target side** (`control_rx_try`, `src/main.cpp`): each control burst is now
  answered with a burst of `ACK_REPEATS = 4` identical ACK copies spaced
  `ACK_GAP_MS = 50 ms` (≈150 ms span), instead of a single packet.
- **Instructor side**: the post-burst blocking ack-listen `CTRL_ACK_LISTEN_MS`
  widened `200 → 300 ms` to cover the burst.

### Root cause being defeated

A phone attached to the instructor makes NimBLE connection events preempt its
loopTask in the gap between TX-complete and re-arming RX (`radio::start_receive()`).
A lone ACK then lands on a momentarily-deaf SX1262 and is lost — and because the
preemption phase is roughly periodic, it missed *systematically* (0/4), not at
random. Repeating the ACK guarantees a copy arrives once RX is armed and outside a
BLE blind spot.

## Stations / hardware

| Role        | Station | Board       | Notes                                  |
|-------------|---------|-------------|----------------------------------------|
| Instructor  | stn73   | Cardputer   | the BLE-connected node (the variable)  |
| Target A    | stn43   | Heltec V4   | Hunter — easy `show` readback          |
| Target B    | stn115  | Wio Tracker | nRF52 target — exercises the other build |
| Fox (opt.)  | stn42   | Heltec V4   | only for the keying-collision check    |

All three control-path builds get the change: `cardputer_adv` (instructor) plus
`heltec_v4` and `wio_tracker_l1` (targets). Reflash all participating stations.

## Tooling

- `tools/blevenv/bin/python scripts/ble_cmd.py <id> "<cmd>"` — a BLE central that
  connects to `MorseStn-<id>`, sends one command, and prints the NUS notify stream.
  **This is the reproduction harness**: driving the relay through it keeps a BLE
  central attached for the whole relay, exactly as a field phone would, and the
  async `ACK …` notification comes back over the same connection.
- `scripts/serial_cmd.py <port> "<cmd>"` — USB serial console driver (the
  known-good 4/4 path), used as the control.

## Reproduction (optional — confirm the bug pre-fix)

Check out the parent commit (single-ACK), reflash stn73 + stn43, and run the BLE
case below. Expect **0/4** ACK lines surfaced while delivery still succeeds. Skip
if we trust the prior 2026-06-18 finding; the value here is proving the fix flips
a previously-failing run on the *same* rig.

## Test cases

### T1 — BLE-attached relay surfaces the ACK (the fix)  ★ primary

The exact failing scenario. With the BLE central attached the whole time:

```
for i in 1 2 3 4; do
  tools/blevenv/bin/python scripts/ble_cmd.py 73 "relay 43 wpm 2$i"
done
```

- **PASS:** an `ACK 43 seq=N: …` line appears in the notify stream on **≥3 of 4**
  runs (target: 4/4). Pre-fix this was 0/4.
- Each run uses a distinct value (`wpm 21..24`) so the target applies a *fresh*
  seq each time (a duplicate seq acks `dup seq N`, which still proves the path but
  muddies "did it really apply"). The synchronous `relaying to id 43 (seq N): …`
  line confirms the command was staged; the later `ACK …` line is what we're
  measuring.
- If `ble_cmd.py`'s reply window closes before the async ACK arrives, raise its
  `reply_timeout` (relay bursts/acks resolve within a few seconds).

### T2 — delivery still succeeds (rule out a regression in command apply)

Independent of ACK return, confirm the command actually landed:

```
tools/blevenv/bin/python scripts/ble_cmd.py 43 "show"     # expect wpm=24
```

- **PASS:** `show` reflects the last relayed value. (Delivery was never the
  failing half; this guards against the multi-ACK loop accidentally breaking it.)

### T3 — USB control path is still 4/4 (no regression)

The previously-good path must stay good — widening the window / bursting ACKs must
not hurt the no-BLE case:

```
# instructor on USB serial; no phone connected
for i in 1 2 3 4; do
  python scripts/serial_cmd.py <instructor_port> "relay 43 wpm 3$i"
done
```

- **PASS:** `ACK 43 seq=N: …` printed on **4/4** runs.

### T4 — nRF52 target acks too (Wio build)

Repeat T1 against **stn115** (`relay 115 wpm 2$i`). Exercises the
`wio_tracker_l1` build of the multi-ACK loop (different `radio::send`/FEM-less
path, `delay()` on nRF52).

- **PASS:** `ACK 115 seq=N: …` surfaces on ≥3/4 BLE-attached runs.

### T5 — instructor stops on the first copy (dedup, no double-count)

The target sends 4 copies; the instructor must treat them as **one** ACK.

- Watch the instructor panel / serial: after a relay it shows **`1 ack(s)`**
  (or `done`), never 2–4 for a single target.
- **PASS:** unicast relay reports exactly one ACK from the target; `instructor_
  service_rx` dedups extra copies by `src_id`.

### T6 — fox keying not disrupted by the ACK burst (only if a fox is live)

The ACK burst spans ~150 ms inside the fox's silent RX window. With **stn42** as a
live fox and **stn43** hunting it, relay a command to the fox and confirm the
hunter still decodes the fox's message cleanly afterward (no garbled copy from the
ACK burst colliding with keystate).

- **PASS:** hunter decode is intact after the relay; no new loss attributable to
  the ACK burst. (The burst fits inside `CTRL_RX_WINDOW_MS = 2500`.)

## Pass/fail summary

The fix is validated when **T1 + T4** flip the BLE-attached case to majority-ACK
(was 0/4) while **T2, T3, T5** confirm nothing regressed. T6 is a
collision-margin check, run it if a fox is on the bench.

## Automation

`scripts/ble_ack_test.py` (BLE venv: `tools/blevenv/bin/python`) runs T1–T5 plus
delivery readback unattended. `--setup` puts stn73 into Instructor mode first.

The harness keeps a BLE central attached for the whole run (the jitter source that
caused #3) and reads the ACK back **over that same BLE notify stream** — the real
operator experience. This is only a valid measurement because the companion BLE
notify-throughput fix is in place; see the trap below for why.

**Measurement trap (history):** before the BLE notify-throughput fix, the ACK could
NOT be measured over BLE — that bug shredded the ACK text down to a bare newline
when a phone was attached, so the ACK *looked* absent over BLE even though it
arrived. The first validation pass worked around it by reading the ACKs off the
**instructor's USB serial** (`Serial.printf` in `instructor_service_rx`). After the
throughput fix (MTU negotiation + notify backpressure + single-write ACK line) the
ACK renders intact over BLE and the harness measures it there directly.

## Results — 2026-06-19 (HW-validated, end-to-end over BLE)

Rig: instructor stn73 (Cardputer) with a live BLE central attached the whole time;
targets stn43 (Heltec V4) and stn115 (Wio Tracker L1, nRF52). `ble_ack_test.py`
**6/6**, ACK measured over the BLE notify stream:

| Check | Result |
|-------|--------|
| T1 BLE-attached relay → stn43, full ACK over BLE | **4/4**, 4 copies each, full text (pre-fix 0/4) |
| T4 BLE-attached relay → stn115, full ACK over BLE | **4/4**, 3–4 copies each, full text (pre-fix 0/4) |
| T5 ACK copies share one seq (dedup) | 4/4 single-seq |
| T3 USB relay, no BLE (control) | **4/4**, no regression |
| T2 delivery readback (stn43 / stn115) | wpm landed on both |

The multi-ACK burst lands in full (3–4 of the 4 copies received per relay) and each
copy now arrives as complete text (`ACK <id> seq=N: wpm = …`) over BLE, confirming
the fix defeats the connection-event blind spot AND that the operator's phone can
read the result. Separately confirmed alongside the throughput fix: `bootlog` over
BLE returns complete (16/16 lines, was truncating to ~#112) and MTU negotiates to
247. **T6 (fox-keying collision) not run** — the bench had both non-instructor
nodes as Hunters, not a fox+hunter pair; the ACK burst fits inside
`CTRL_RX_WINDOW_MS` so collision risk is low, but it remains an unrun margin check.

Note: `set_wpm` clamps at 40, so use sub-40 distinct values for unambiguous
delivery readback (the harness uses 12–15 / 22–25).

## Record on completion

- Update `DONE.md`: flip the `[~]` BLE-attached ACK gap bullet to `[x]` with the
  measured ratios and date.
- Update memory `instructor-ack-lost-over-ble` (and `MEMORY.md`) from
  "NOT HW-validated" to validated with the numbers.
