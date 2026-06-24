# Plan: rename `bcast` → `alert` + audible attention tone (overrides mute)

Status: **IMPLEMENTED + HW-VALIDATED** on branch `feat/instructor-broadcast` —
all 5 envs build clean. Validated 2026-06-17 on the bench (instructor=Cardputer
stn73, Hunters=Heltec V4 stn43 + Wio stn115): `scripts/alert_tone_test.py` 6/6 —
both muted Hunters sounded the ~1.5 s tone (mute overridden), mute stayed on
afterward, and `alert clear` was silent; tone also confirmed audibly by the
operator. The rename path is covered by `scripts/broadcast_display_test.py` 7/7.
Builds on the instructor broadcast banner (`docs/plan-instructor-broadcast.md`,
shipped commit `5c0d2e0` / `d6fbaaa`).

Internal `bcast`/`broadcast` C++ symbols were left unchanged (only the
operator-facing command, console strings, docs, and scripts say `alert`), per the
"Code symbol naming" recommendation below.

## Goal

When the instructor pushes a message to every station, receivers should **sound
an attention tone** in addition to showing the banner — so an operator who isn't
looking at the panel still notices. The tone must sound **even when the device is
muted** (`mute on`), because a broadcast is by definition important.

## Decisions (locked)

1. **Rename the console command `bcast` → `alert`** (and `bcast clear` →
   `alert clear`). Update all docs and test scripts.
2. **Every alert beeps.** No opt-in flag.
3. **Remove the `-a` CLI option** — there's no longer a non-alert variant to opt
   out of. The `BCAST_FLAG_ALERT` wire bit is **always set** now (every broadcast
   is an alert); the receiver doesn't branch on it (it's vestigial for behavior
   since commit `d6fbaaa`, where `set_banner()` ignores it — `(void)alert;`,
   `src/main.cpp:1492`), but the sender sets it so the packet is self-describing.
4. **Sound = a single long attention tone** (~1.5 s steady), reusing the board's
   existing sidetone frequency. (Distinct pitch / "CQ CQ" Morse explicitly
   rejected for v1 — see "Rejected / deferred".)
5. The tone **overrides mute transiently** — it does not change the persisted
   `config::muted()` state; after the tone ends the node is muted again.

## Wire format: frozen

Keep the on-air packet **byte-identical** so we don't have to reflash for
compatibility and don't disturb the validated broadcast path:

- `proto::MAGIC_BCAST = 0x42` and the 4-byte header (`magic, src, seq, flags`)
  stay exactly as-is (`src/protocol.h:275-285`).
- The `flags` byte stays in the struct. The sender now **always sets
  `BCAST_FLAG_ALERT` (1)** — every broadcast is an alert — and the receiver still
  ignores the bit for behavior (every alert beeps regardless). Keeping the bit set
  (rather than 0) makes the wire self-describing and leaves room for a future
  non-alerting broadcast type to use `flags = 0`.

So this change is **CLI + audio + docs only**, not a protocol change.

## Code symbol naming

The user-facing surface (command word, console output, docs, scripts) **must**
become `alert`. The internal C++ symbols (`BroadcastMsg`, `broadcast_rx_try`,
`g_bcast`, `set_banner`, `BCAST_*`) are an optional, separate rename:

- **Recommended:** leave the internal symbols named `bcast`/`broadcast` to keep
  the diff small and low-risk (the feature is HW-validated under those names).
  Add a one-line comment that the operator-facing name is now "alert".
- If a clean rename is wanted, do it as a **separate mechanical commit** after the
  behavior change lands and re-runs green, so a rename bug can't hide behind a
  behavior bug.

## Implementation

### 1. Sidetone API — add a mute-bypassing attention gate

`src/sidetone.h`: add

```c
// Attention gate for instructor alerts. Forces the sidetone ON regardless of the
// master mute (sidetone_set_mute) and regardless of any in-progress key/RX tone,
// at the board's fixed sidetone frequency. Used only by the broadcast/alert path;
// the main loop times the duration and calls sidetone_alert(false) to end it.
void sidetone_alert(bool on);
```

Each backend gates output on `(s_on && !s_muted)`; change every gate to
`(s_alert || (s_on && !s_muted))` and add `s_alert` + the setter. Sites:

- `src/sidetone.cpp` — **two** implementations in this file (selected by build
  flag): the LEDC PWM path (gate at `:33`, `apply()`) and the I2S/MAX98357A
  envelope path (gate at `:126`). Add `s_alert` to both; the setter calls
  `apply()` (LEDC) / lets the feeder pick it up (envelope), mirroring
  `sidetone_set_mute` at `:55` / `:195`.
- `src/sidetone_nrf52.cpp` — **two** implementations: the I2S envelope path
  (gates at `:133` and `:160`) and the buzzer/PAM8403 path (`buzzer_start()` at
  `:342`, mute setter `:329`). Mirror `sidetone_set_mute` (`:466` / `:329`).
- `src/sidetone_cardputer.cpp` — M5.Speaker path. `sidetone_on()` currently does
  `if (!s_muted) M5.Speaker.tone(...)` (`:52`). `sidetone_alert(true)` must call
  `M5.Speaker.tone(s_freq, UINT32_MAX)` unconditionally; `sidetone_alert(false)`
  re-evaluates: if a key/RX tone is held and not muted keep it, else
  `M5.Speaker.stop()`.

All 5 envs have working sidetone hardware, so every board beeps. (Watch the
Cardputer amp-brownout history — `M5.Speaker.begin` is deferred to
`sidetone_init`; the alert path only calls `.tone()`, so no new init risk.)

### 2. main.cpp — alert-tone state machine (non-blocking)

Near the banner state (`src/main.cpp:444`):

```c
static constexpr uint32_t ALERT_TONE_MS = 1500;   // attention tone length
static uint32_t g_alert_tone_until = 0;           // millis() deadline (0 = off)
```

A helper, ticked from `loop()` regardless of mode (next to where `draw_banner` /
banner state is serviced):

```c
static void alert_tone_tick(uint32_t now) {
    if (g_alert_tone_until && (int32_t)(g_alert_tone_until - now) <= 0) {
        g_alert_tone_until = 0;
        sidetone_alert(false);
    }
}
static void start_alert_tone(uint32_t now) {
    g_alert_tone_until = now + ALERT_TONE_MS;
    sidetone_alert(true);
}
```

Non-blocking (no `delay`), so the watchdog feed and radio RX are unaffected.

### 3. Trigger points

- **Receiver:** in `broadcast_rx_try()` (`src/main.cpp:1519`), inside the existing
  `if (fresh)` block (after `set_banner(...)`), call `start_alert_tone(now)` —
  **but only when `b.text[0]` is non-empty**, so `alert clear` (empty text =
  dismiss) does NOT beep. Dedup by seq is already handled here, so each alert
  beeps exactly once even though the instructor bursts the same seq 5×.
- **Instructor local echo:** at the send site (`src/main.cpp:781`,
  `set_banner(arg, ...)`), also `start_alert_tone(millis())` when not clearing, so
  the instructor gets the same feedback. (If the instructor should stay silent,
  skip this one call — noted as the single place to change that policy.)

### 4. CLI rename + drop `-a`

`src/main.cpp:750-787` — the command handler block:

- Match `"alert"` / `"alert "` instead of `"bcast"` (`:750`); `arg = line + 6`.
- Delete the `-a` parse (`:760-761`) and the `alert` local; always send
  `flags = proto::BCAST_FLAG_ALERT` (`:775`) — every broadcast is an alert.
- `alert clear` keeps the empty-text dismiss behavior (`:762-763`).
- Update usage/echo strings: `? alert <text...> | alert clear` (`:767`),
  `bcasting (seq …)` → `alerting (seq …)` (`:785`), drop the `" ALERT"` suffix.
- The `mode != MODE_INSTRUCTOR` guard message → `alert: only in Instructor mode`.

`show` output: the `banner =` line (`:897`) can stay, or rename to `alert =`.

## Edge cases / interactions

- **Mute is restored automatically** — we only flip `s_alert`, never
  `config::muted()`. After `ALERT_TONE_MS` the node is muted again.
- **Hunter actively decoding Morse:** RX keying drives `s_on` via
  `set_tone(rx_down)` (`:1809`). With the `s_alert || …` OR gate, the alert forces
  a steady tone for its duration and normal keying resumes cleanly afterward.
- **Overlapping alerts:** a second fresh alert just re-arms
  `g_alert_tone_until` — the tone extends, never stacks.
- **`alert clear`** dismisses banners everywhere and is silent (text empty).

## Docs & scripts to update

- `docs/commands.md` — the `bcast` entry → `alert`; document the audible tone +
  mute override; remove `-a`.
- `docs/plan-instructor-broadcast.md` — add a back-reference noting the command
  was renamed to `alert` and now beeps (don't rewrite history; just a pointer).
- `scripts/broadcast_display_test.py` and `scripts/broadcast_test.py` — every
  `bcast …` the instructor sends becomes `alert …`; the display test's
  `f"bcast {BANNER}"` (`broadcast_display_test.py:148`). Optionally add an
  assertion that a muted Hunter still sounds the tone (observable via a new
  `# alert: tone` debug log — see below). Consider renaming the scripts to
  `alert_*` for discoverability (optional).
- `scripts/README.md` — the broadcast test descriptions.
- `README.md` / `DONE.md` / GitHub Issues — wherever the broadcast banner menu/feature
  is listed, reflect the rename + audible alert.
- `CLAUDE.md` / memory note `instructor-broadcast-banner.md` — update the command
  name.

## Test / observability

- Add a gated debug log on the receiver when the tone fires, e.g.
  `if (g_debug) Serial.printf("alert: tone %ums\n", ALERT_TONE_MS);` in
  `start_alert_tone`, so the bench harness can assert the beep without a mic.
- New bench check: `mute on` both Hunters, send one `alert`, assert each logs
  `alert: tone` (proves mute was overridden) and that mute is still `on`
  afterward (`show`).
- Re-run `scripts/broadcast_display_test.py` (renamed) to confirm the banner-
  display + hold behavior is unchanged by the rename.
- HW-validate on at least one ESP32 (Heltec, MAX98357A I2S path), the Cardputer
  (M5.Speaker), and one nRF52 (buzzer or I2S) since the mute-gate edit touches
  every backend.

## Rejected / deferred

- **"CQ CQ" in Morse / distinct alert pitch** — rejected for v1. CQ needs a Morse
  encoder in the alert path and a distinct pitch needs per-backend runtime
  frequency control (the I2S/envelope backends fix frequency at init). A single
  steady tone at the existing frequency is unmistakable and ~5 lines per backend.
  Revisit if operators want to distinguish alert types by ear.
- **Per-board EIRP / loudness tuning** — out of scope; the tone uses the current
  `vol` level (but overrides mute).
