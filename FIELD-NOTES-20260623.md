# Field Notes — 2026-06-23

First real exercise of the **on-device standalone Cardputer Instructor UI**
(branch `feat/instructor-standalone-ui`, Phases 0–3) driving a live fleet. The
UI itself worked (menu, fox-id persistence, the four actions compose + send —
see §4). What the test surfaced were **three receive-side firmware problems**,
two of them crashes, triggered when the instructor sent mute / unmute / a
broadcast over the air.

**Framing — these are not UI bugs.** The on-device menu only *composes the same
command strings* (`relay <id> ...`, `relay 255 mute on|off`, `alert <text>`)
that the BLE/serial console has always sent, and feeds them to the same
`handle_setup_command()`. So the faults below live in the **receiving** stations'
(pre-existing) firmware and were simply exercised for the first time. Nothing
here is implemented yet — this is the post-exercise writeup that feeds `TODO.md`.

Stations involved: instructor = stn73 (Cardputer ADV). Targets = stn43
(Heltec V4), stn65 (Heltec V3), stn26 + stn115 (nRF52: RAK/Wio).

---

## 1. Heltec V3/V4 reboot-LOOP on the first tone after startup (stn43, stn65)

**Status: NOT root-caused. Two hypotheses tried and disproved (below). Paused
2026-06-23 — this section is the full record of what is known.**

**Observed (full set of empirical facts).**
- After receiving **UNMUTE** (`relay 255 mute off`), stn43 (V4) and stn65 (V3)
  dropped into a **reboot loop**. It is a loop only because `mute` is persisted
  (`config::set_muted`): once unmuted, every boot comes up unmuted and replays
  the crash.
- The crash is **the first full-volume tone after a cold start**, and it happens
  **most of the time** — it is **probabilistic**, not every single boot.
- It is **independent of how long after boot the first tone occurs.** It still
  crashes when the board **joined during a pause and had several seconds of idle**
  (radio up, BLE up, everything settled) before the first tone fired.
- **Once any tone has played successfully, ongoing loud tones never crash** for
  the rest of that power cycle. The failure is specifically the *first* real
  (non-silence) tone after power-up.
- **Physically disconnecting the MAX98357A amp avoids the crash** (boots fine).
- **Full-volume tones are fine in normal/steady-state use, on both battery and
  USB.** Only the first tone after a cold start is at risk.

**Hypothesis 1 — steady-state volume vs. supply: DISPROVED.** Full volume is fine
in normal use on both supplies, so the supply can sustain the steady-state
current of a full-volume tone. (Also: a `vol` low/high sweep was discussed as the
test — the "fine in normal use at full volume" observation already settles it.)

**Hypothesis 2 — boot-transient current stack-up (first tone coincides with the
boot-init current peak: SX1262 cal, NimBLE bring-up, 240 MHz before the
end-of-setup 80 MHz drop): DISPROVED by experiment.** We added a **warm-up gate**
(force the sidetone muted for `SIDETONE_WARMUP_MS = 1000` ms measured from the
**end** of `setup()`, after radio/BLE are up and the core has dropped to 80 MHz;
release in `loop()`). Built + flashed to both boards (`b1bb7c1-dirty`). **It did
NOT fix the crash.** Combined with the observation that it crashes even with
several seconds of idle before the first tone, the cause is **not** a timing
overlap with boot transients — delaying the first tone does not help.

**Current best understanding (unconfirmed): a one-time, load-dependent transient
on the very first time the amp drives real output after power-up.** The signature
— first-tone-only, probabilistic, timing-independent, gone once any tone
succeeds, present only with the amp connected — points to a one-shot turn-on
event of the class-D output stage the first time it slews to high amplitude, not
to anything about *when* that happens. After the first successful drive,
something is "conditioned" (rail/amp transient settled) and it never recurs that
power cycle. The exact mechanism is **not** pinned and **not** scoped.

**Electrical / schematic facts (confirmed, still valid).**
- **MAX98357A filterless class-D** (datasheet `reference/MAX98357A-MAX98357B.pdf`:
  "filterless … eliminates output filtering … single bypass capacitor"). Board
  schematic `reference/MAX98357A-schematic-only.png`: **10 µF + 0.1 µF VDD→GND
  bypass**; **220 pF + ferrite beads (FB1/FB2) on the speaker lines** = EMI
  filter, **not** an output coupling cap. So there is **no output DC-blocking cap
  to charge** — there is no classic output-cap inrush. (The first-tone transient,
  whatever it is, is not that.)
- VDD is on the **shared 3V3 rail** (`docs/components/max98357a.md`), same rail as
  the ESP32-S3. Full-swing 750 Hz into 4 Ω at 3.3 V ≈ 1.3–1.4 W ≈ **~450 mA**.
- **A reset-reason capture was never obtained** — so even "brownout" is unproven;
  it could be a hard fault or a glitch coupling into the MCU on the first
  high-amplitude drive. Getting this capture is now the most important next step.
- For completeness: **mute does not save/restore volume.** I2S `sidetone_set_mute`
  is just `s_muted = m;`; the feeder writes silence while muted and resumes at the
  unchanged boot gain `s_gain_q15 = config::volume()*1024`.

**Candidate fixes to try next (when work resumes), most promising first.**
- **First-tone amplitude soft-start.** Ramp the sample gain from ~0 up to target
  over ~100–300 ms **on the first real tone after power-up** (and after a long
  idle), instead of the current ~8 ms envelope, so the amp eases into its first
  high-amplitude output rather than stepping to it. This directly targets the
  one-time first-drive transient and fits "once one tone plays, the rest are
  fine." Cheapest software thing left to try. Trigger it **only** on the first
  tone after power-up / long idle, not every element (inter-character gaps are
  ~700 ms at 5 wpm, so a per-onset ramp would smear normal Morse).
- **One-time boot "conditioning" tone.** Deliberately play a single **very
  low-amplitude** tone once at end of boot (below the damaging threshold) so the
  one-time first-drive event happens quietly/safely, and the first *audible* tone
  is effectively the "second" tone. Pre-arms the amp.
- **Hardware / measurement.** Scope **3V3 and the amp VDD on the first tone vs. a
  later tone** to actually see the transient and confirm brownout-vs-other; try
  **more bulk capacitance on the amp VDD** (buffer the first-drive surge) or a
  separate/decoupled amp supply; consider sequencing the **SD** pin.
- **Loop-breaker (mitigation, not a fix).** On a boot whose reset reason is
  brownout, come up **muted** one-shot so a unit cannot wedge itself in the loop
  over the air. (Depends on the reset-reason capture actually reading brownout.)

**Retained design requirements (independent of the root cause).**
- **Alert tone must soft-start too**, and should play at **MAX volume**, then
  **restore the original mute + volume**: *save (mute, volume) → force unmute +
  set MAX → soft-started alert tone → restore (mute, volume).* From the amp's
  point of view the alert is itself a "first tone after idle."
- **Normal unmute must re-apply the configured volume** (today it only clears the
  mute flag) — especially once the alert path can leave the volume at MAX.

**Recovery (field).** Boot with the amp disconnected → `mute on` → reconnect amp
→ reboot (comes up muted, stable). Or mute before any fox is on the air so no
first tone fires. Note the first tone *may* succeed (~probabilistic), so a single
clean boot is not proof a unit is safe.

**Open questions (highest-value first).**
- **Capture the reset reason** on the actual crash (brownout vs hard-fault vs
  other). Use `scripts/serial_capture.py` across the reboot. NOTE: on the **V4**
  a panic backtrace prints to **UART0 / GPIO43, not USB** (`docs/debug-heltec-v4.md`),
  so USB capture may show only the reset, not the cause.
- **Scope** 3V3 and amp VDD during the first tone vs. a later tone.
- Why **probabilistic** ("most of the time")? Suggests a phase/timing-dependent
  marginal transient.
- Is it truly the amp output-stage turn-on, or something in the **I2S/DMA
  first-non-zero-block** path on the MCU side?

**Code state.** The **warm-up gate experiment was REVERTED 2026-06-23** (it did
not fix the problem). It had forced the sidetone muted for `SIDETONE_WARMUP_MS`
from the end of `setup()` (`main.cpp`: `SIDETONE_WARMUP_MS` /
`g_sidetone_warmup_until`, released in `loop()`); the gate is gone from the tree.
Any future first-tone soft-start work starts fresh from the candidate fixes
above, not from this gate.

**Code anchors.** `sidetone.cpp` (I2S feeder, `s_gain_q15`, `sidetone_set_mute`);
boot order in `main.cpp` `setup()` (`sidetone_init` ~L1375, radio init ~L1391,
`set_cpu_low_power` last ~L1456, warm-up stamp after `watchdog_begin` ~L1464);
`apply_mute` L301.

---

## 2. nRF52 (RAK/Wio) reboot on RECEIVING the mute command (stn26, stn115)

**Observed.** On receiving the broadcast **MUTE** (`relay 255 mute on`), both
nRF52 stations (stn26, stn115) **rebooted** (single reboot, not the §1 loop).
Different hardware from §1: these use a **piezo buzzer** (`-DSIDETONE_BUZZER`,
`sidetone_nrf52.cpp`), **no MAX98357A** — so this is unrelated to the amp
brownout.

**Status: ROOT-CAUSED + MITIGATED 2026-06-23. It was BLE-related, NOT the LittleFS
hypothesis below.** The reboot is the nRF52 `ble_provision` stop()/begin() cycle:
on a state change the BLE-AUTO panel-follow policy cycles BLE, and `begin()` after
`stop()` re-runs `Bluefruit.begin()`/`bleuart.begin()` against an
already-initialized SoftDevice and **hangs → 8 s watchdog reset** (not the radio
RX path). Mitigated by pinning `BLE_CYCLE_UNSAFE` (these boards stay `BLE_ON`
always); the real fix — making nRF52 `stop()`/`begin()` safe to cycle, then
dropping the pin — is tracked in `TODO.md` under **"Restore BLE-AUTO idle power
saving on nRF52 (RAK4631/Wio Tracker L1)"**.

**Original hypothesis (DISPROVED, kept for the record).** It looked like the
mute-specific work `apply_mute() → config::set_muted()` — a **LittleFS flash
write** (`kv_nrf52.cpp`) — executing **inside `control_rx_try()`**
(`main.cpp:1788`), i.e. in the radio RX critical section between the control
packet and the 4× ACK TX burst. That was the prime suspect but turned out not to
be the cause; the fault is the BLE cycle above. (The buzzer mute path
`sidetone_set_mute` → `buzzer_stop`, `sidetone_nrf52.cpp:330`, is benign.)

**Code anchors.** `ble_provision_nrf52.cpp` stop()/begin(); `BLE_CYCLE_UNSAFE`
pin in `main.cpp` setup(). (Disproved-path anchors: `control_rx_try`
`main.cpp:1788`; `apply_mute` L301; `config::set_muted`; `kv_nrf52.cpp`.)

---

## 3. Instructor locked out for ~90 s after a broadcast command

**Status: FIXED + HW-VALIDATED 2026-06-23** (flashed to stn73, broadcast now frees
the menu in ~6 s instead of 90 s). New `CTRL_BCAST_WINDOW = 6000` ms gives
broadcast (255) relays a short fire-and-forget window; the give-up check in
`loop_instructor()` selects the window by target (broadcast → `CTRL_BCAST_WINDOW`,
unicast → full `CTRL_BURST_WINDOW`). At `CTRL_PROBE_INTERVAL` 1500 ms that's ~4
probe bursts to reach all listeners. The give-up branch still prints the ack tally.

**Observed.** After issuing a **broadcast** command (mute), the instructor could
not send a new instruction **for a long time**.

**Root cause (confirmed from code).** `CTRL_BURST_WINDOW = 90000` ms = **90
seconds** (`main.cpp:58`). A relay keeps `g_ctrl.active` set for the whole burst
window while it collects ACKs; for a **broadcast** (`target_id = 255`) the
instructor **cannot know how many ACKs to expect**, so it waits out the *entire*
90 s window instead of finishing early on delivery. The on-device menu entry is
guarded on `!g_ctrl.active && !g_bcast.active`, so for those 90 s the operator is
**locked out** of sending anything new.

**Proposed fixes.**
- Treat **broadcast (255) relays as fire-and-forget** / short window (like
  `alert` already is), rather than waiting the full unicast ACK window.
- And/or let **staging a new command preempt** an in-flight one (abort `g_ctrl`
  and start the next), so the operator is never blocked by a send already done.
- Optionally surface the broadcast **ACK tally** (`g_ctrl.acks_from[]` already
  collects it) instead of silently waiting.

**Code anchors.** `CTRL_BURST_WINDOW` `main.cpp:58`; `loop_instructor` burst
window / give-up branch (~L2833); menu-entry guard in `handle_cardputer_key`.

---

## 4. Standalone Cardputer Instructor UI — worked (positive note)

**Observed.** The new on-device UI (Phases 0–3) ran on stn73: the menu opens on a
keypress and closes on ENTER/timeout, the fox id persists (first-run forcing +
Settings editor), and the four actions (fox power / fox message / mute / alert)
compose their command strings and hand off to the async relay/alert path without
freezing the loop. The crashes above are all on the **receiving** side.

**UX tweaks already made this session.** Larger menu font (text size 2, shortened
labels); the fox-id editor titled `Settings -> Fox ID` to read as a settings
page.

**Follow-ups for the UI itself.**
- ~~It depends on §3 being fixed, otherwise a broadcast mute/alert locks the menu
  for 90 s.~~ **§3 fixed 2026-06-23** — broadcast now frees the menu in ~6 s.
- Consider an on-screen "sending…/N acks" state so the lockout is at least
  legible rather than a frozen menu. (Less urgent now §3 caps it at ~6 s.)
