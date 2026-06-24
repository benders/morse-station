# Plan: Standalone Cardputer Instructor (no paired phone)

Status: **PLANNED** — phases defined and approved (see §Implementation phases),
not started. This document is the design + risk register; nothing here is built
yet. Decided: the runtime menu lives in a **new** `instructor_ui_cardputer.cpp`
(not an extension of the boot editor), and the RETURN alert text is **typed at
send time** (no stored preset).

## Goal

Make the **M5Stack Cardputer ADV** usable as a self-contained Instructor station
in the field, driven entirely by its **onboard keyboard + LCD**, with **no BLE
phone and no USB laptop** attached. Today Instructor mode is real and proven, but
it is driven only as a *text console* over BLE-NUS or USB serial — you type
`relay <id> <cmd>`, `alert <text>`, etc. (`docs/commands.md`, "Instructor (remote
control over GFSK)"). The keyboard is currently wired only to a boot-time
callsign/message editor (`config_ui_cardputer.cpp`) and a single runtime `m`
mute-toggle key (`src/main.cpp` ~L2901). There is no on-device way to compose and
send instructor commands.

## Field usage context (drives every design choice)

- **One fox, one unit, the whole exercise.** The fox's station id is fixed for
  the session → store it once, don't make the operator retype it per command.
- **Hunters addressed as a group.** Mute/unmute goes to broadcast id `255`; the
  instructor never needs to enumerate individual hunter ids.
- **Four primary actions, in rough order of the exercise:**
  1. **Set fox TX power** at the start — `relay <foxid> pwr <0..3>` (LO/MED/HI/MAX,
     `PWR_LEVELS` in `main.cpp:112`).
  2. **Update the fox message** — `relay <foxid> msg <text>`.
  3. **Mute / unmute hunters** — `relay 255 mute on` / `relay 255 mute off`.
  4. **Send the "return to camp" alert** at the end — `alert <text>` (sticky
     banner + attention tone on every node).
- **Power matters, but the duty cycle is tiny.** The instructor only transmits
  when the operator issues a command, and only needs the radio listening during a
  command's ACK window. Between commands it can sleep deeply. This is a much
  friendlier power profile than Fox/Hunter (which run flat-out) — see §Power.

## Key architectural insight (keeps this low-risk)

**Every instructor action already exists as a text command in
`handle_setup_command()`** (`relay`, `alert`, `mute`, `pwr`, `msg`, …) and the
entire GFSK burst/ACK/banner machinery already runs asynchronously in
`loop_instructor()`. So the on-device UI does **not** re-implement any radio
logic. It is purely a **front-end that composes a command string and feeds it to
`handle_setup_command(line, Serial)`** — exactly what the BLE/serial paths do. The
proven async path (staging into `g_ctrl` / `g_bcast`, the burst window, the
blocking ACK-listen, the sticky banner) is reused verbatim.

This means the feature decomposes into two mostly-independent tracks:
- **Track 1 (UI):** a runtime keyboard/LCD menu that builds command strings.
- **Track 2 (Power):** sleep the node between commands.

Track 1 is shippable on its own and delivers the headline feature (no phone).
Track 2 is the higher-risk add-on and can land later without changing Track 1.

---

## Track 1 — On-device Instructor UI

### Design constraints

- **Must be non-blocking at the loop level.** `loop_instructor()` services inbound
  ACKs, runs the burst window, and animates the banner. The UI cannot sit in a
  blocking `wait_key()` *while a command is in flight* or it will starve
  `instructor_service_rx()` and miss ACKs. **However**, modal blocking *while
  idle* (no `g_ctrl.active`, no `g_bcast.active`) is acceptable and far simpler —
  the instructor does nothing else while the operator is navigating a menu or
  typing. The clean rule: **edit modally while idle; once a command is staged,
  return to `loop()` and let the existing async path run.**
- **Reuse `config_ui_cardputer.cpp` primitives** (`edit_field`, `draw_field`,
  `wait_key`, `keyboard::read_char`) rather than inventing a new input layer.
- **Coexist with the existing idle draw.** `loop_instructor()` already owns the
  screen when idle (`display::instructor(... "ready", "relay <id> <cmd>")`). The
  menu replaces that idle prompt; after a command completes, control returns to
  the existing idle/ack draw.

### Proposed UI shape (a runtime menu)

A top-level menu reachable by any keypress while the instructor is idle (mirrors
how the boot editor opens). Entries map 1:1 to the four field actions plus setup:

```
INSTRUCTOR
1  Fox power      (LO / MED / HI / MAX)     -> relay <foxid> pwr <n>
2  Fox message    (edit text, send)         -> relay <foxid> msg <text>
3  Mute hunters / Unmute hunters (toggle)   -> relay 255 mute on|off
4  RETURN alert   (type text, send)          -> alert <text>
5  Settings: fox id, instructor TX power (ipwr)
ESC/idle -> back to ready screen
```

- **1 Fox power:** a 4-way picker (LO/MED/HI/MAX). Selecting stages
  `relay <foxid> pwr <n>` and returns; the existing ACK display shows whether the
  fox confirmed.
- **2 Fox message:** open `edit_field` seeded with the last sent message, ENTER
  stages `relay <foxid> msg <text>`. (Store the last message locally so it's
  pre-filled, not blank, each time.)
- **3 Mute toggle:** no text entry; stages `relay 255 mute on|off`. Track the
  intended state locally for the label (the fleet has no single source of truth,
  so this is best-effort — see Risk D).
- **4 RETURN alert:** `edit_field` opened empty (or seeded from the last alert
  sent this session, RAM-only — not persisted), ENTER stages `alert <text>`. The
  operator types the alert message at send time; there is no stored preset.
- **5 Settings:** edit/persist the fox id (NVS) so 1/2 don't need it typed; show
  instructor's own `ipwr`.

### Wiring it in

- New `instructor_ui_cardputer.cpp` exposing `config_ui::instructor_menu()`,
  called from `loop_instructor()` when idle and a key is pending — **not** the
  boot-only `config_ui::run()`. (Decided: a separate file, so the boot editor and
  the runtime instructor menu stay separate concerns.)
- The menu builds a `char line[]` and calls `handle_setup_command(line, Serial)`.
  Because the relay/alert commands are **Instructor-mode-gated already**
  (`mode != MODE_INSTRUCTOR` guards at L854/L896), no new auth surface is added.
- Persist: **fox id** (new NVS key via `config::set_*`) and **last fox message**
  (reuse `config::fox_message()`). The alert text is **not** persisted — it is
  typed each time it is sent.

Track-1 work is sequenced in §Implementation phases (Phases 0–4), Track-2 in
Phases 5–7.

---

## Track 2 — Power: sleep between commands

The instructor's duty cycle is near-zero: transmit on a keypress, listen briefly
for an ACK, otherwise idle. Today the loop runs flat-out at 80 MHz with BLE pinned
**on** (instructors boot `BLE_ON`, `main.cpp:2958`) — the worst case for an idle
field device. Opportunities, cheapest/safest first:

1. **Drop BLE entirely in standalone mode (biggest, safest win).** A
   keyboard-driven instructor needs no BLE central. `ble off` already reclaims
   **~70 mA on the V4** (memory: BLE NimBLE advertising is the dominant idle
   draw). Add a policy so a Cardputer instructor defaults BLE **off** (it's still
   reachable via its own keyboard). This alone may be most of the available
   savings and carries almost no risk.
2. **Blank the LCD backlight when idle.** `display::tick()` already blanks after
   the idle timeout; the LCD backlight is a large Cardputer draw. Ensure the
   instructor participates in idle-blank (instructors keep the panel logic but
   verify blank actually cuts the backlight on this board). Low risk.
3. **CPU light-sleep between commands (higher risk — see Risk A).** Gate strictly:
   sleep **only** when `!g_ctrl.active && !g_bcast.active && display::blanked()`.
   Wake on **keyboard INT** (`PIN_KBD_INT` = GPIO11, which is RTC-capable on the
   ESP32-S3, so it can drive `ext0`/`gpio` light-sleep wake) and a short timer
   fallback. Because the instructor only ever needs the radio during its own
   command's ACK window (the relay path already does a blocking ACK-listen, and
   the burst campaign keeps the node fully awake while `g_ctrl.active`), sleeping
   between commands loses **no** inbound traffic — the instructor never receives
   anything unsolicited.

Track-2 work is sequenced in §Implementation phases (Phases 5–7).

---

## Implementation phases

Ordered, one commit per phase after it builds (`cardputer_adv` plus the other
envs, since the shared files compile everywhere). The two gating risk
mitigations are pulled to the front; cleanup of existing code is attached to the
phase that touches it rather than deferred.

### Phase 0 — De-risk text entry (GATING — build nothing on top until done)
Covers Risk B and the prerequisites for any keyboard-heavy UI. Must precede the
text-entry phases (3b RETURN alert, 3a fox message).
- **0a — Panic capture.** Wire the reconnecting-serial panic dump (`TODO.md` C2)
  so a typing crash is recorded, not just silently rebooted. The reset-reason log
  already exists.
- **0b — Harden the input layer.** Audit the TCA8418 FIFO drain in
  `keyboard_cardputer.cpp` and the editor buffer bounds in `edit_field()`.
  Exercise heavy typing on HW to reproduce-or-rule-out the C2 crash before
  building on it. Treat this as a gate for the text-entry steps.
- **Cleanup (own commit, pure refactor):** `wait_key`/`draw_field`/`edit_field`
  are file-static in an anonymous namespace in `config_ui_cardputer.cpp`. Since
  the new `instructor_ui_cardputer.cpp` needs the same primitives, **extract them
  into a shared `kbd_ui` namespace** (`kbd_ui.h`/`.cpp`) so the boot editor and
  the runtime menu share one input layer instead of duplicating it. Boot-editor
  behavior unchanged.

### Phase 1 — Runtime menu skeleton (was U1)
- New `instructor_ui_cardputer.cpp` + `config_ui::instructor_menu()` decl in
  `config_ui.h`. Renders the 5-item menu; ESC/timeout returns to the ready draw;
  **no actions yet**.
- Entry from the `loop_instructor()` idle branch (`main.cpp` ~L2814, the
  `!g_ctrl.active` block) only when a key is pending and
  `!g_ctrl.active && !g_bcast.active` (the Risk-C boundary). The menu may block
  while idle; ENTER stages and returns to the async path.
- **Cleanup:** generalize the `config_ui.h` header doc (it currently describes
  only "callsign/message configuration"). Factor the hardcoded idle prompt
  `display::instructor(..., "ready", "relay <id> <cmd>")` (at `main.cpp:2825–2826`
  and the ack path ~L2722) into one ready-screen helper, since the menu replaces
  that prompt and Phase 2 adds the fox id to it.

### Phase 2 — Fox id setting + persistence (was U2 / Risk E)
- Add a fox-id NVS key following the exact `cached_* + KEY_*` pattern in
  `config.cpp` (mirror `KEY_CSEQ`). Default to a sentinel (e.g. 0) that forces
  first-run setup; refuse to stage relays until set; show it on the ready screen.

### Phase 3 — Action items (was U3–U6), one commit each
Each just builds a `char line[]` and calls `handle_setup_command(line, Serial)` —
no radio logic added.
- **3-pwr — Fox power picker** → `relay <foxid> pwr <n>`; confirm via the existing
  ACK display.
- **3-msg — Fox message editor** → `relay <foxid> msg <text>`, seeded from
  `config::fox_message()` (reuse, no new key). *(Gated on Phase 0.)*
- **3-mute — Mute/unmute toggle** → `relay 255 mute on|off`; local intent label
  (Risk D — label as "muted (sent)", not ground truth).
- **3-alert — RETURN alert** → `alert <text>`, **typed at send time** (empty or
  seeded from the last alert sent this session, RAM-only; no NVS preset).
  *(Gated on Phase 0.)*
- **Cleanup:** the `#ifdef DEVICE_CARDPUTER_ADV` keypress block inside `loop()`
  (`main.cpp:2904–2920`) already overloads one keypress with wake / banner-dismiss
  / latched-alert / `m`-mute. Adding "any key opens the instructor menu when idle"
  would make it worse — **factor it into a `handle_cardputer_key()` helper** so
  the menu-entry and the existing mute/wake/banner logic live together.

### Phase 4 — Field validation (was U7)
End-to-end against the real fox + ≥1 hunter (`heltec` hunter + cardputer
instructor): power change audible/visible, message copied, mute reaches hunters,
alert banner+tone on every node. No code unless bugs surface.

### Phase 5 — Power: BLE-off default for Cardputer instructor (was P1)
- Default a Cardputer instructor to **BLE off** instead of the current
  `g_ble_mode = BLE_ON` pin at `main.cpp:1438`; add a `show` line. Measure idle
  current before/after (memory: BLE advertising ≈ 70 mA on the V4).
- **Cleanup (significant):** the loop's BLE-AUTO branch carries a long comment
  block and an explicit `mode != MODE_INSTRUCTOR` guard (`main.cpp:2958–2961`)
  whose entire justification is "instructors are pinned BLE_ON." Once a Cardputer
  instructor no longer pins ON, **re-examine and simplify that guard and its
  comment** — the instructor can fall through to normal AUTO panel-follow (it is
  reachable via its own keyboard now), removing a special case. Verify a
  Heltec/serial-driven instructor still behaves.

### Phase 6 — Power: confirm idle-blank cuts the LCD backlight (was P2)
- Verify `display::tick()` idle-blank actually kills the Cardputer LCD backlight;
  measure. Low risk; likely no code beyond a backlight-off call if blank doesn't
  reach it.

### Phase 7 — (stretch / defer) Light-sleep between commands (was P3 / Risk A)
Per §Open decisions: ship Phases 1–6, measure battery, then decide. If pursued:
- Bench-prove wake-on-keypress via `PIN_KBD_INT` (GPIO11, RTC-capable) **before**
  any UI depends on it.
- Gate sleep strictly on `!g_ctrl.active && !g_bcast.active && display::blanked()`;
  do not power the codec down — reuse lazy bring-up (memories
  `cardputer-boot-pop-fixed`, `cardputer-amp-brownout`).
- **Cleanup:** `PIN_KBD_INT` is currently parked `INPUT_PULLUP` and never used;
  if P3 is built, give it a real wake owner. Keep the pin define in
  `platformio.ini` per the project pin-map rule.

### Cleanup/simplification summary
| Touched code | Cleanup | Phase |
|---|---|---|
| `config_ui_cardputer.cpp` anon-ns input primitives | Extract `wait_key`/`draw_field`/`edit_field` into shared `kbd_ui` | 0 |
| `config_ui.h` doc comment | Generalize beyond "callsign/message config" | 1 |
| Hardcoded `"ready" / "relay <id> <cmd>"` idle strings (×2 sites) | Single ready-screen helper showing fox id | 1–2 |
| `loop()` `#ifdef DEVICE_CARDPUTER_ADV` keypress block | Factor into `handle_cardputer_key()` | 3 |
| BLE-AUTO branch + `mode != MODE_INSTRUCTOR` guard & comment | Remove the instructor special-case once BLE no longer pinned on | 5 |
| `PIN_KBD_INT` parked-but-unused | Give it a wake owner (or leave documented) | 7 |

---

## Riskiest areas (ranked)

### Risk A — Light-sleep × M5Unified × radio/codec (HIGHEST)
ESP32-S3 light-sleep is the riskiest single item and the most uncertain.
- **USB CDC dies across light-sleep** (known). Fine in the field, but it kills
  serial debugging *of the sleep path itself* — capture via the NVS reset-reason
  log and the LCD, not serial.
- **M5Unified owns the LCD + ES8311 codec.** This project already has a history of
  codec power touchiness — boot pop and amp brownout
  (memory: `cardputer-boot-pop-fixed`, `cardputer-amp-brownout`). Sleep/wake
  cycling risks re-introducing pops or a brownout on wake. Mitigate: don't power
  the codec down for sleep; bring it up lazily as already done for sidetone.
- **Wake source validation.** `PIN_KBD_INT` (GPIO11) is RTC-capable, and the
  TCA8418 is already configured to assert INT on keypress (`CFG_KE_IEN`,
  `keyboard_cardputer.cpp`). But the INT line is currently only parked as
  `INPUT_PULLUP` and never used as a wake — its asserted level/latching behavior
  through a sleep cycle is unverified. Bench-prove wake-on-keypress *before*
  building UI on top of it.
- **Decision to capture:** *Is light-sleep worth it, or do P1 (BLE off) + P2
  (backlight) already get the field runtime we need?* Recommendation: ship Track 1
  + P1/P2 first, measure, and only do P3 if the battery numbers demand it. The
  1750 mAh cell with BLE off and the backlight blanked may already last a full
  exercise day.

### Risk B — The unexplained keyboard typing crash (HIGH)
`TODO.md` Stage C2 records an **intermittent crash + reboot while typing on the
keyboard** (seen once on HW 2026-05-31, never reproduced, never root-caused;
suspects: TCA8418 FIFO/keynum decode in `keyboard_cardputer.cpp`, or the editor
buffer in `config_ui_cardputer.cpp`). This UI leans **heavily** on the keyboard,
including message/alert text entry mid-exercise. A crash here is a field failure.
- **Mitigate:** before/while building U4–U6, add the reconnecting-serial panic
  capture described in C2, exercise heavy typing, and harden the editor buffer
  bounds and FIFO drain. Treat reproducing-or-ruling-out this crash as a gating
  task for the text-entry steps. The watchdog will at least reboot (and the
  reset-reason log will record it), but a reboot mid-alert is still bad.

### Risk C — Non-blocking vs. modal editing (MEDIUM, design)
The existing `config_ui::run()` is **blocking and boot-only**. Naively reusing it
at runtime would block `loop_instructor()` and stall ACK servicing. The plan's
rule — *edit modally only while idle, return to the async path to send* — resolves
this, but it must be implemented deliberately: the menu/editor may block, but it
must **not** be entered while `g_ctrl.active`/`g_bcast.active`, and ENTER must
*stage and return*, never spin waiting for the ACK. Getting this boundary wrong
either drops ACKs or makes the UI feel frozen during a burst.

### Risk D — Fleet state has no single source of truth (MEDIUM)
Mute/unmute and power are fire-and-(best-effort)-ack: the instructor shows
`ACK <id>: <reply>` if the target confirms, but there's no authoritative "are the
hunters muted?" readout. The UI's mute label is a *local intent*, not ground
truth — if a hunter missed the command, the label lies. For broadcast `mute 255`
there's no per-hunter ack tally surfaced today.
- **Mitigate:** label as intent ("Hunters: muted (sent)"), and lean on the
  existing repeat/ack behavior. Optionally surface the broadcast ack tally that
  `g_ctrl.acks_from[]` already collects. Acceptable for v1.

### Risk E — Discoverability / first-run fox id (LOW-MEDIUM)
Commands need the fox id. If it's unset or wrong, every relay silently goes
nowhere. Mitigate with U2: force fox-id setup on first run, show it persistently
on the ready screen, and refuse to send relays until it's set.

### Risk F — Small screen text entry (LOW)
240×135 with long fox messages / 40-char alert limits. `edit_field` overflows
rather than wraps/scrolls. Mitigate with simple horizontal scroll of the edit
buffer (the banner draw already scrolls long text). Cosmetic, not correctness.

---

## Open decisions (captured; none blocking — pick during implementation)

1. **Light-sleep (Phase 7): build it or not?** *Recommendation:* defer; ship
   Phases 1–6 (UI + BLE-off + backlight-blank), measure battery, decide. (Risk A.)
2. ~~**New file vs. extend `config_ui_cardputer.cpp`.**~~ **DECIDED:** new
   `instructor_ui_cardputer.cpp` (+ `config_ui::instructor_menu()` decl), with the
   shared input primitives extracted into `kbd_ui` (Phase 0) so the boot editor
   and the runtime instructor menu stay separate concerns without duplication.
3. **Should the instructor menu also exist for the Heltec** (button-only, no
   keyboard)? Out of scope here — Heltec has no keyboard; this plan is
   Cardputer-specific. A Heltec instructor stays phone/serial-driven.
4. **Mute target granularity** — broadcast `255` only (v1) vs. per-hunter lists.
   *Recommendation:* broadcast-only for v1 (matches "mute the hunters" intent).
5. **Does keyboard config grow to cover wpm/farns** for the fox via relay too?
   *Recommendation:* keep v1 to the four field actions + settings; add more relay
   verbs to the menu later if asked.

## Files touched (anticipated)

- `src/instructor_ui_cardputer.cpp` (new) + `src/config_ui.h` (decl) — the menu.
- `src/kbd_ui.{h,cpp}` (new) — shared `wait_key`/`draw_field`/`edit_field`
  primitives extracted from `config_ui_cardputer.cpp` (Phase 0).
- `src/main.cpp` — call the menu from `loop_instructor()` idle; factor the
  Cardputer keypress block into `handle_cardputer_key()` (Phase 3); BLE-off
  default for a Cardputer instructor + simplify the AUTO guard (Phase 5); sleep
  gate (Phase 7).
- `src/config.{h,cpp}` — NVS key: fox id (last message reuses `fox_message()`; no
  alert-preset key — alert text is typed at send time).
- `src/keyboard_cardputer.cpp` / `config_ui_cardputer.cpp` — harden against the
  Risk-B typing crash; optional INT-wake support for Phase 7.
- `docs/commands.md` — document the on-device instructor UI.

## What this plan deliberately does NOT do

- No change to the on-air protocol — it reuses `relay`/`alert`/`mute`/`pwr`/`msg`
  exactly as they are on the wire today.
- No change to Fox/Hunter/Livekey behavior.
- No Heltec instructor UI (no keyboard there).
