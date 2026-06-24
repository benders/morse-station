# Morse Station — Completed Work

Archive of finished items. The fox-hunt firmware: a
stationary **fox** transmits a Morse description of its location; **hunters**
decode it by ear and on the display. Primary platform **Heltec WiFi LoRa 32 V4**
(ESP32-S3 + SX1262); the **M5Stack Cardputer ADV** is an experimental second fox
sharing one `src/` tree. See `docs/` for the protocol, command reference, and
per-component notes; open work is tracked in
[GitHub Issues](https://github.com/benders/morse-station/issues).

---

## Stage 0 — Groundwork

- [x] Project builds and flashes to Heltec V4 (PlatformIO `heltec_v4` env).
- [x] RSSI band scanner to picture the 902–928 MHz noise floor for channel
      selection.

## Stage 1 — Audio out (sidetone)

- [x] 600 Hz (later 750 Hz) sidetone on the MCU via the ESP32 LEDC peripheral
      (square wave; S3 has no true DAC).
- [x] Tone on/off behind `sidetone_on()` / `sidetone_off()` so the key handler
      and RX decoder can both drive it.
- [x] Flashed and confirmed a clean tone on hardware (GPIO4 → PAM8403; cap not
      needed for a clean tone).

## Stage 2 — Telegraph key input

- [x] Key on a GPIO with `INPUT_PULLUP`, shorts to GND. Confirmed on **GPIO6**
      (GPIO5 read LOW even floating — pull-up wouldn't hold).
- [x] Software debounce (5 ms settling) in `src/morsekey.cpp`.
- [x] Key down → `sidetone_on()`, key up → `sidetone_off()`. Verified on HW:
      clean tone, no latency.

## Stage 3 — Radio link bring-up (FSK TX/RX)

- [x] Runtime-switchable TX/RX (hold PRG at boot = TX beacon, else RX listener).
- [x] GFSK link on a single fixed channel (905.0 MHz), TX/RX of a short payload
      with RSSI.
- [x] RadioLib SX1262 FSK config — 4.8 kbps, 5 kHz dev, sync `{0x2D,0xD4}`,
      rxBw 39 kHz, low power (+2 dBm). (See `docs/protocol.md`.)
- [x] **V4 external FEM (PA+LNA) brought up** — `radio::fem_power_on()` powers
      VFEM (GPIO7), enables the FEM (CSD), sets PA-bypass, and `setDio2AsRfSwitch`
      switches TX/RX on the V4.2. Without it RX was dead (~−107 dBm). One image
      drives the superset of V4.2 (GC1109) and V4.3.1 (KCT8103L) pins.
- [x] **V4.3 RX LNA fixed** — the KCT8103L CTX line (GPIO5, `PIN_FEM_CTX`) is
      software-driven and doubles as RX LNA select (LOW=LNA, HIGH=bypass). The
      old static-HIGH left V4.3 RX permanently LNA-bypassed.
      `fem_set_rx()`/`fem_set_tx()` now track radio state around send/receive.
      Confirmed on HW. (See `docs/components/heltec-v4.md`.)
- [x] Bench link verified both directions, huge margin (~−40 dBm two floors
      away).

## Stage 3a — Field-tuning punch list

- [x] Station ID fixups: **21301 → 43**, **21101 → 42** (provisioned into NVS).
- [x] Sidetone **600 → 750 Hz** (`TONE_HZ`).
- [x] Keying slowed to **15 WPM**, WPM settable in NVS (`config::wpm()`, key
      `wpm`, default 15, clamp 5..40; `wpm <n>` / `provision.sh --wpm`).
- [x] **Farnsworth timing**, Farnsworth speed settable in NVS
      (`config::char_wpm()`, key `char_wpm`, default 18, clamp wpm..40);
      `morse::Player::begin(wpm, char_wpm)` stretches gaps via the ARRL formula.
- [x] Longer end-of-message delay (REPEAT_PAUSE 3000 → 7000, later 12000 ms).
- [x] **Hunter decoder mis-read the slower/Farnsworth timing** — fixed by
      broadcasting `wpm`/`char_wpm` in the `Ident` packet (sent each message loop
      + the 8-min cadence). `morse::Decoder::begin(wpm, char_wpm)` derives
      Farnsworth-aware thresholds; the hunter retunes its decoder + watchdog from
      the heard Ident. Live-keying still uses local config speeds.
- [x] Hunter display: single rolling copy line shows the tail of the buffer (last
      21 glyphs = full 128px width).
- [x] Hunter header shows `RECV <id>` (station ID of the last received
      KeyState/Ident, populated on first signal) plus `radio::frequency_mhz()`
      (905.0 MHz) right-justified.
- [x] Fox default power on boot → **LO** (`pwr_idx = 0`).
- [x] Added a **MAX** power level (`{"MAX", 22}`); +22 dBm is the SX1262 chip
      ceiling. (On the Heltec the FEM PA is not switched per level — see TODO.)
- [x] Boot splash shows the **git revision** (`scripts/git_rev.py` injects
      `-DGIT_REV`; shown for 2 s and printed to serial).
- [x] **Persist fox TX power in NVS** (`config::fox_pwr_idx`, key `fox_pwr_idx`,
      default 0=LO; restored on boot, saved on each PRG tap).
- [x] **Hunter dit/dah display mode** — PRG tap toggles the copy line between
      decoded letters and raw dit/dah elements (`morse::Decoder::last_elements()`).

## Stage 4 — Fox message loop

- [x] Fox message stored in firmware (`FOX_MSG`), later in NVS.
- [x] Text → Morse timing via `morse::Player` (non-blocking timed key-state
      stream feeding both sidetone and radio).
- [x] Local sidetone driven from the Morse timing; verified by ear.
- [x] Loops with a configurable `REPEAT_PAUSE`.
- [x] Added `morse::Decoder` (key timing → text), used by RX in Stage 5/6.

## Stage 5 — Morse over FSK (keystate broadcast)

Decided on **keystate broadcast** (carries live keying *and* the canned fox loop
identically). See `docs/protocol.md`.

- [x] TX sends a 5-byte `KeyState` packet every 30 ms (magic, station id, key
      state, seq). RX reconstructs timing and drives its sidetone. No ACKs.
- [x] Fox loop feeds the keystate stream from `morse::Player` — same wire format
      as a live key.
- [x] RX: key stream → sidetone + `morse::Decoder` → text.
- [x] End-to-end verified: fox transmit → hunter hears Morse + decodes text.

## Stage 6 — Fox-hunt integration

- [x] Fox mode: loops the message, transmits keystate, "FOX TX" status.
- [x] Hunter mode: receives, plays sidetone, shows decoded text + RSSI bar.
- [x] Live-key mode (students key to each other after the hunt).
- [x] Mode select at boot — PRG-button menu (short = cycle, long = select, idle
      auto-select).
- [x] **Hunter volume tracks RSSI** (analog "tune for max volume"): RSSI mapped
      over −110..−40 dBm onto the sidetone duty. Curve is **exponential
      (dB-linear)** — a linear map was inaudible. Run the fox at LO so RSSI
      varies. Verified on HW.
- [x] **Fox adjustable TX power** — PRG tap cycles LO/MED/HI (+MAX added later)
      via `radio::set_tx_power` (SX1262 chip output). Verified on HW.
- [x] **Hibernate** (power-off stand-in) — boot-menu item powers down FEM +
      peripheral rail and deep-sleeps with no wake source; RST restarts into the
      menu. Verified on HW.
- [x] **RX stuck-key on signal loss fixed** — a watchdog in `loop_hunter` forces
      key-up after ~one dah with no keystate (`RX_TIMEOUT_MS`). Verified on HW.
- [x] **dit/dah display is per-element** (each `.`/`-` scrolls as the decoder
      classifies it on the key-up edge); dit/dah is the default view. Letters
      separated by a space, words by `" / "`.
- [x] **Text scrolling freeze fixed** — the rolling buffer's "drop oldest half"
      off-by-one dragged the NUL terminator mid-buffer at ~127 chars, severing the
      string. Trim now moves relative to `len` and re-terminates (shared
      `rolling_append()`); copy line widened 16 → 21 glyphs. Verified on HW.
- [x] **Battery meter display** — `battery::percent()` (device-split: Heltec
      divider on GPIO1, gate GPIO37 active-HIGH; Cardputer via
      `M5.Power.getBatteryLevel()`) rendered in the Hunter/Fox/Live-key headers on
      both platforms. (commit dc5c429)
- [x] **Hunter button cycles volume** (was a view toggle) — PRG/USER cycles
      MUTE → LOW → MED → HIGH (I2S levels `{4,11,32}`), persisted; buzzer/PAM8403
      boards toggle mute only. dit/dah is the default copy view; the new
      `showtext on|off` console command (serial/BLE/relay) switches to decoded
      text, replacing the old button view-toggle. (commits 216ea97/718bca9)
- [x] **Fox `stop`/`start` TX commands** — runtime `g_tx_halted` flag in the
      shared Fox/Live-Key emission path halts/resumes all air output (keystate/edge
      + Ident); works over serial/BLE/relay, `show` reports `tx=running|halted`.
      Runtime-only (reboot comes up transmitting). HW-validated stn38. (commit 0367463)

## Stage 7 — Range & polish

- [x] Per-unit station IDs in NVS (`src/config.cpp`; random id on first boot,
      override via `set_station_id()`).
- [x] Default station ID derived from the eFuse MAC (XOR-folded into 1..254) —
      stable per unit, no stored value; override still persists.
- [x] Callsign + fox message in NVS, provisioned over serial (boot console
      `call`/`msg`/`id`/`show`/`done` + `scripts/provision.sh`). Verified on HW
      (both units to KC8HOB).
- [x] **Field provisioning via BLE** (Nordic UART Service) — see
      `docs/commands.md`. No custom phone app needed (iOS BLE-GATT compatible).
      - [x] **Step 0 — parser refactor.** `handle_setup_command(line, Print&)` is
            pure dispatch; `run_setup_console()` reads a Serial line and calls it.
      - [x] **Step 1 — NimBLE NUS peripheral.** `src/ble_provision.{h,cpp}`
            advertises `MorseStn-<id>`, `BleOut : Print` over TX-notify, RX
            `onWrite` assembles a CR/LF line. Verified round-trip from macOS.
      - [x] **Step 2 — always-on live control.** NimBLE stays up the whole
            session; RX `onWrite` queues lines, `process()` dispatches from
            `loop()`. Live apply (params + TX power) behind `g_live_apply`; mode
            changes via `mode <n>` + `reboot`. Re-advertises on disconnect.
            Verified on HW swapping the fox role entirely over BLE.

## Diagnostics

- [x] **Boot/crash reason history in NVS.** `setup()` records each boot's
      `esp_reset_reason()` into a 16-entry NVS ring buffer (`boot` namespace:
      `log` blob + `lh` head; `cnt` stays the monotonic boot counter) instead of
      keeping only the single previous reason — so the cause survives even when
      several reboots land back to back. New `bootlog` console/BLE command dumps
      the ring (oldest first) without serial attached at crash time; `bootlog
      clear` empties it. Makes the intermittent Cardputer typing crash and the
      native-USB panic-loses-backtrace problem diagnosable after the fact.
      (`src/main.cpp` `bootlog_record`/`bootlog_dump`/`handle_setup_command`;
      `docs/commands.md`.) Builds clean on both `heltec_v4` and `cardputer_adv`.

- [x] **Hardware watchdog (field resiliency).** A hardware watchdog reboots a
      wedged node in the field instead of leaving it silently dead. Behind the
      `platform::` seam: ESP32 uses the Task WDT (`esp_task_wdt` on the Arduino
      `loopTask`), nRF52 uses the `NRF_WDT` peripheral clocked off the always-on
      32.768 kHz LFCLK (keeps counting even if the CPU/SoftDevice/an ISR wedges).
      Armed for **8 s** at the end of `setup()` (`watchdog_begin`) and fed at the
      top of `loop()` (`watchdog_feed`); a timeout triggers a full chip reset that
      shows up in the next boot's `bootlog` as the watchdog cause (`TASK_WDT` on
      ESP32, `WATCHDOG` on nRF52). On the Adafruit nRF52 bootloader the hardware
      reset cause does **not** survive (it clears `RESETREAS` and wipes `.noinit`
      RAM), so the nRF52 reason is recovered at the app layer from a persisted
      flash **reboot-intent** flag (`HAS_REBOOT_INTENT`): every boot re-arms it to
      `RUNNING`, and a clean `reboot`/`hibernate` stamps `SOFT`/`OFF` — so an
      *unexpected* reset (watchdog, crash, brownout) is the only thing that boots
      back with `RUNNING` still set ⇒ reported as `WATCHDOG`. New always-compiled
      `stall [secs]` console command spins `loop()` with no WDT feed to prove the
      watchdog actually fires (`scripts/watchdog_test.py` automates the round
      trip). **Hardware-validated** on the Heltec boards (ESP32 Task WDT) and on
      the Wio Tracker L1 (nRF52 `NRF_WDT`: `stall 12` → reset at 8.3 s → next boot
      `reason=2(WATCHDOG)`). (`src/platform_esp32.cpp` / `src/platform_nrf52.cpp`
      `watchdog_begin`/`watchdog_feed`; `src/main.cpp`; `docs/commands.md`.)

## Instructor (remote control over GFSK)

- [x] **Instructor mode** (`MODE_INSTRUCTOR`, `mode 3`) — relays the full console
      command set to a distant fox/hunter over GFSK control packets
      (`MAGIC_CTRL`/`CACK`, instructor = reserved id 0), so an exercise leader can
      re-tune a fox beyond BLE range. `relay <id|255> <cmd>` bursts in the fox's
      silent inter-message window (silence-synced to a unicast target) and shows
      `ACK <id>: <reply>`. End-of-message `Listen` beacon (`MAGIC_LISTEN`) lets the
      instructor burst immediately instead of waiting out `CTRL_SILENCE_MS`.
      HW-validated 2026-06-10 (stn73 Cardputer → fox42/hunter43). Full mechanics in
      `docs/commands.md`. (commits f4a2381 / 0abff75 / 2516351)
  - [x] **BLE-attached ACK gap (#3).** With a phone connected to the instructor
        over BLE the command still landed but **no** ACK was ever surfaced (0/4):
        NimBLE connection events preempt the instructor's loopTask in the gap
        between TX-complete and re-arming RX, so the target's lone ACK flew past a
        momentarily-deaf SX1262 — and because the preemption phase is ~periodic it
        missed *every* time. Fix: the target now answers each control burst with a
        small **burst of ACKs** (`ACK_REPEATS=4`, `ACK_GAP_MS=50`) so at least one
        copy lands once RX is armed and outside a BLE blind spot, and the
        instructor's blocking ack-listen widened `200→300 ms` to cover the burst.
        The instructor stops on the first copy (dedups by `src_id`); the rest are
        harmless. (`src/main.cpp` `control_rx_try` + `CTRL_ACK_LISTEN_MS`/
        `ACK_REPEATS`/`ACK_GAP_MS`.) **HW-validated 2026-06-19** with a phone (BLE
        central) attached to instructor stn73, driving both target builds —
        `scripts/ble_ack_test.py` 6/6: BLE-attached relay to stn43 (Heltec V4) and
        stn115 (Wio nRF52) now lands the ACK **4/4** (3–4 copies each, pre-fix 0/4),
        each relay's copies share one seq (clean dedup), USB control path still 4/4,
        and `show` confirms delivery on both targets. Test plan in
        `docs/test-ble-ack-gap.md`. After the BLE notify-throughput fix below, the
        whole suite was **re-run measuring the ACK over BLE** (the real operator
        path) — 6/6, full ACK text intact 4/4 to both targets — so #3 is now
        verifiable end to end on a phone, not just on the instructor's USB serial.
- [x] **BLE notify throughput — long replies / async ACKs dropped chunks.** Replies
      longer than a few hundred bytes (`bootlog`, full `help`) arrived truncated
      over BLE NUS (tail lost), and an async relay ACK arrived as a bare newline —
      `BleOut::emit` fired back-to-back 20-byte `notify()` calls with no MTU
      negotiation and no backpressure, so chunks were silently dropped when they
      outran the NimBLE msys mbuf pool. Three-part fix in `src/ble_provision.cpp`:
      (1) request a 247-byte ATT MTU (`NimBLEDevice::setMTU`) and track the
      negotiated value via `onMTUChange`, chunking to `MTU-3` instead of 20 — far
      fewer notifications per reply; (2) real backpressure — `notify()` returns
      false when the pool is momentarily empty, so retry with a short `delay()`
      yield (runs on the main loop task, never the host task) instead of dropping;
      (3) coalesce `ble_provision::notify()` into a SINGLE `write()` of `"line\n"`
      — two back-to-back notifications raced and the *first* (the text) was the one
      dropped, which is why the async ACK showed up as a lone `\n`. HW-validated
      2026-06-19 on stn73 (Cardputer): MTU negotiates to 247, `bootlog` returns
      complete (16/16 lines through #128, was truncating ~#112), and the relay ACK
      renders in full over BLE (`ACK 43 seq=N: wpm = …`, all multi-ACK copies).
      The nRF52 `BleOut` (`ble_provision_nrf52.cpp`, Adafruit Bluefruit) got the
      analogous fix: `Bluefruit.configPrphBandwidth(BANDWIDTH_MAX)` before
      `begin()` (MTU 247 + deeper HVN queue), explicit chunking to `getMtu()-3` in
      `emit()` — Bluefruit's unbuffered `BLEUart::write()` forwards the whole
      buffer to `_txd.notify()`, which sends ONE ≤MTU notification and silently
      drops the rest while returning success, so the chunking has to be ours —
      per-chunk backpressure on the `write()==0` (queue-full) return, and the same
      single-write `notify()` coalesce. HW-validated 2026-06-19 on stn115 (Wio):
      MTU 247, `bootlog` complete (17/17), and a ~380-byte `help` line that
      previously truncated mid-string now returns whole (matches USB).
- [x] **Instructor display + power policy** — OLED header always shows battery and
      the current TX level; instructor boots at MAX so every field fox hears the
      bursts, with a non-persisted `ipwr <0..3>` runtime override for the bench. The
      PRG button wakes the display only (no power cycling), and BLE is pinned ON
      even when the panel blanks so the instructor stays reachable.
      (commits f5107c4 / 9718a22)
- [x] **Instructor alert** (`alert <text>` / `alert clear`) — pushes a plaintext
      banner to **every** station's screen (distinct from `relay`, which runs a
      console command silently). Its own packet (`MAGIC_BCAST`, fire-and-forget,
      5 repeats, per-seq dedup), magic-gated so old nodes drop it. The banner
      force-wakes a blanked panel and holds it lit ~60 s (centered / two-line wrap
      / marquee scroll); button or keypress dismisses; `show` reports the active
      banner. Every alert also sounds a **~1.5 s attention tone that overrides a
      receiver's `mute`** for its duration without changing the persisted mute
      state — so an operator who isn't looking still notices. New mute-bypassing
      `sidetone_alert()` gate added to all three sidetone backends (ESP32 LEDC +
      I2S/MAX98357A, Cardputer M5.Speaker, nRF52 buzzer + I2S); non-blocking
      `ALERT_TONE_MS` timer in `loop()`. HW-validated 2026-06-17 (instructor
      Cardputer stn73 → Hunters Heltec V4 stn43 + Wio stn115):
      `scripts/broadcast_display_test.py` 7/7 (banner wakes + holds the panel) and
      `scripts/alert_tone_test.py` 6/6 (muted Hunters sound the tone, mute stays
      on afterward, `alert clear` silent). See `docs/plan-alert-tone.md` /
      `docs/plan-instructor-broadcast.md`. (commits 5c0d2e0 / d6fbaaa / 327b0cd /
      7622ead)

## Power saving

- [x] **Idle power saving** (`docs/commands.md` — `screen`/`power`/`hibernate`).
      60 s idle blanks the OLED (`display::tick`/`activity`; `screen` reports/forces
      state), the ESP32-S3 idles at **80 MHz** (`platform::set_cpu_low_power` at
      setup; `power` reports the running clock), and `hibernate` now also puts the
      SX1262 to sleep (`SetSleep`). BLE-UART follows panel state (~70 mA idle saving
      on V4) and the wake press is swallowed so waking a blanked panel doesn't fire
      the button. The Cardputer stays at 240 MHz (80 MHz crashes M5Unified).
      HW-validated on V3 (stn38 Fox) and V4 (stn43 Hunter).
      (commits 63d9dc9 / 5781fa6 / ece1e14 / 30f3ac2)

- [x] **Hunter BLE auto-shutdown despite an always-lit panel** (`docs/commands.md`
      — `ble`). The `ble auto` policy above followed `display::blanked()`, but a
      Hunter's panel never blanks while receiving (inbound RX keying calls
      `display::activity()` on every edge/text/sync packet), so BLE never
      reclaimed its ~70 mA on a busy node. Fix: a separate `g_ble_operator_ms`
      timer, touched only by genuine operator actions (button, console/BLE
      command, keyboard) and NOT by RX/banner activity; Hunter's `auto` branch
      gates on that timer (60 s) instead of the panel, with a connected central
      exempted. Every other mode is unaffected (still follows the panel).
      Flashed + booted on all 6 bench units (stn26/38/42/43/73/115).

---

## Cardputer ADV port (experimental fox)

Shares one `src/` tree with the Heltec, selected by `-DDEVICE_CARDPUTER_ADV`.
See `docs/components/cardputer-adv.md`.

### Hardware facts / decisions

- [x] **Cap LoRa-1262 TCXO = 1.8 V confirmed on HW** — `beginFSK()` succeeds and
      the cap decoded real off-air Morse as a hunter.
- [x] **M5Unified for audio + display.** `M5.Speaker` drives the ES8311 sidetone;
      `M5.begin()` claims the LCD so display uses `M5.Display` (M5GFX). LovyanGFX
      dropped from `lib_deps` (M5GFX ships with M5Unified). All via
      `cardputer_m5_begin()`.

### Stage C0 — Build + groundwork

- [x] Device-split `pins.h` (Heltec / Cardputer blocks, `#error` if neither).
- [x] `radio.cpp` device-aware: FEM guarded by `HAS_FEM` (Heltec only); Cap pins
      + DIO2 RF switch for the Cardputer.
- [x] Sidetone: LEDC path guarded to Heltec; new `sidetone_cardputer.cpp`
      (M5.Speaker), same API.
- [x] Display: U8g2 path guarded to Heltec; new `display_cardputer.cpp` on the
      240×135 panel via M5.Display.
- [x] `main.cpp` hibernate FEM/Vext pins guarded by `HAS_FEM`.
- [x] platformio `cardputer_adv` env: RadioLib + M5Unified.
- [x] **`pio run -e cardputer_adv` builds clean** — resolved M5Unified API drift
      (Speaker `tone`/`stop`, `TFT_*` colour macros). Both `heltec_v4` and
      `cardputer_adv` envs build clean.

### Stage C1 — Fox bring-up on hardware

- [x] Flash + boot OK: `M5.begin()` (LCD + ES8311) comes up with no crash; boots
      through splash + menu.
- [x] **Radio init succeeds** — `beginFSK()` OK as hunter and fox; as a hunter
      the cap decoded real off-air Morse, proving SPI pins + TCXO + sync + freq.
- [x] Serial provisioning REPL works over USB-CDC; added `mode <0..2>` to set the
      default boot mode without the G0 button.
- [x] Sound + text confirmed good on HW (M5.Speaker sidetone + fox message
      render); fox loop runs as KC8HOB.
- [x] **Display flicker fixed** — render into a full-frame `M5Canvas` back buffer
      and `pushSprite` once per frame. Stable on HW (~100 s, heap flat, no
      reboots). Boot now logs its reset reason to NVS (`# boot #N reason ...`).
- [x] **Fox runs silent** — local sidetone disabled in fox mode (a beeping
      transmitter is easy to find). Applies to both platforms.

### Stage C2 — Keyboard entry

- [x] TCA8418 driver (`keyboard_cardputer.cpp`): register init (7×8 matrix, event
      mode), polled event FIFO over **M5.In_I2C**, keynum→(row,col) decode, 4×14
      keymap with shift/capslock. (See the internal-I2C re-begin lesson in
      `docs/components/cardputer-adv.md`.)
- [x] On-screen text-entry editor (`config_ui_cardputer.cpp`): field editor
      (append / backspace / ENTER-commit), reused for callsign and message.
- [x] Boot hook: `config_ui::run()` after the splash — a "press any key" window
      opens a 1=Callsign / 2=Message menu; writes go through the same NVS keys as
      the serial console.

### Mute / volume (partial)

- [x] **Mute done** — `sidetone_set_mute(bool)` (device-split: M5.Speaker stop on
      the Cardputer, LEDC/ISR gate on the Heltec; both remember the held key so
      unmute resumes), persisted as `config::muted()`, applied at boot, driven by
      the Cardputer `'m'` key and the BLE/serial `mute [on|off]` command.
      (Graduated `vol <0..n>` still open — see TODO.)

---

## Stage 6 — Fox-hunt integration (more)

- [x] **Always-ACK-at-MAX (field note 4, 2026-06-20).** Remote command of the fox
      worked every time, but the ACK was only received when the instructor was
      close: the command reaches the fox at its good RX sensitivity, but the ACK
      went out at the fox's *current* TX power (typically Low) and died at distance.
      Fix: the ACK path (`control_rx_try`, `src/main.cpp`) saves `pwr_idx`/`g_pa_on`,
      bumps to MAX (`set_tx_power(PWR_LEVELS[N_PWR-1])` + `set_pa(true)`) strictly
      around the `ACK_REPEATS` burst, then restores — command stays at play power,
      acknowledgement goes at full power. Tiny infrequent packet → negligible
      §15.249 cost. Builds heltec_v4 (FEM) + cardputer_adv.
- [x] **Sticky alert + fox halt on alert (field note 6, 2026-06-19).** HW-VALIDATED
      17/17 (commits 693a4b1 / 77e8754 / 09a62e3, branch feat/sticky-alert-fox-halt).
      A received alert carrying `proto::BCAST_FLAG_STICKY` latches: `banner_active()`
      short-circuits on `g_alert_latched` (never expires), the panel is held awake,
      and a Fox/Live-Key node halts TX (`g_tx_halted`). Released only by
      `alert clear`, `start` (TX only — keeps the banner), or reset (RAM-only). The
      instructor does NOT latch its own panel (must stay usable to send the clear);
      local presses can't dismiss a latched alert. KEY FIX (77e8754): the
      fire-and-forget alert burst missed the Fox's once-per-cycle RX window, so the
      campaign is now listen-synced (bursts on the fox Listen beacon) + time-bounded
      (`ALERT_CAMPAIGN_MS`). Test: `scripts/alert_sticky_test.py`.
- [x] **RECV id + RSSI bar clear after timeout.** `loop_hunter` (`src/main.cpp`
      ~L2609) clears `rx_station_id = -1` and the RSSI bar once `now - last_signal`
      exceeds `signal_timeout_ms` (3 s) — the fox's inter-message gap is longer, so
      a stale reading doesn't linger after the station goes quiet.
- [x] **Ignore other stations while locked to one fox.** `loop_hunter` keeps a
      `locked_fox` id (`src/main.cpp` ~L2290); `fox_lock()` admits packets only
      from the locked fox until `last_fox_rx` ages past `signal_timeout_ms`, then
      it re-locks to the next fox heard. So a second station can't hijack the
      copy while the current fox is live.

## Stage 7 — Range & polish (more)

- [x] **Text-frame canned-message mode (field note 7, 2026-06-20).** HW-VALIDATED
      18/18 via `scripts/msg_text_test.py`. A second message mode for canned clues:
      transmit the clue as a plaintext `MAGIC_TEXT` frame + retransmit burst (like
      the ACK/broadcast bursts) and render Morse *locally* on the hunter, instead of
      streaming `EdgeEvent` timing edges that lose a character to a single dropped
      edge (field note §5). `proto::TextMsg`/`MAGIC_TEXT` + `encode_text`/`decode_text`
      (`src/protocol.h`); `config::msgmode`/`set_msgmode` NVS-backed default keyed;
      `msgmode keyed|text` console verb; `tx_text()` (`TEXT_REPEATS`=4 @
      `TEXT_GAP_MS`=50) + `loop_fox` text-cycle branch (gated by rx-window/
      `g_tx_halted`); `loop_hunter` `decode_text` branch dedups by seq and renders
      via a hunter-side `morse::Player`. Builds heltec_v4 + cardputer_adv +
      wio_tracker_l1; forward-compat (old-fw nodes ignore MAGIC_TEXT). Two bugs
      found+fixed in validation: (1) local player started+updated same loop →
      `now-seg_start_` underflow finished it instantly w/ no audio (drive player off
      millis()); (2) clue render (~16 s) outlasts REPEAT_PAUSE(12 s) so each resend
      restarted it (don't restart a render already sounding the same clue). Design:
      `docs/plan-text-message-mode.md`. Option B (ControlCmd⊕text merge) deferred.

---

## Text-mode sync/DF beacon + per-element reveal (field note §8)

Fixed the three field regressions `msgmode text` introduced (hunters out of sync,
no continuous RSSI for DF, per-character not per-element display). Design:
`docs/plan-text-sync-beacon.md`; branch `feat/text-sync-beacon` (merged c70c4fc).
**§8 COMPLETE** — S1–S5 + freeze-fix done & HW-validated; manual-only left
(audible 2-hunter unison by ear, antenna-pull free-run/recover). Tests:
`scripts/sync_beacon_test.py` (S4, 11/11), `scripts/sync_reveal_test.py` (S5),
`scripts/morse_elems_test.sh` (freeze regression).

- [x] **S1** `morse::Player` absolute-position refactor: `position_ms()` +
      `resync(now, pos_ms)`. Keyed/§7 paths byte-identical. (d1f01ae)
- [x] **S2** `proto::Sync` / `MAGIC_SYNC` (0x53) / `POS_IDLE` + `encode_sync` /
      `decode_sync`. Magic-gated, forward-compatible. (678e570)
- [x] **S3** Fox `loop_fox` text branch: silent master render clock, `tx_sync()`
      beacon every `BEACON_MS`=200 ms across render+pause, `TextMsg` re-send every
      `TEXT_RESEND_MS`=2 s. (faed020; clock fix 66fe8b8 — render clock must use
      `millis()`, not the loop's stale cached `now`, or every beacon carries POS_IDLE.)
- [x] **S4** Hunter `loop_hunter`: `decode_sync` branch — presence/RSSI refresh,
      timing adopt, free-run + slack-bounded `resync` (slack = one dit),
      `want_text_seq` mid-join + seek to live `pos_ms` on the next `TextMsg`
      (aa70561). HW-VALIDATED 11/11 (`scripts/sync_beacon_test.py`): beacon cadence
      ~212 ms median across render+pause, both hunters slave the same clue seqs
      with |drift| within one dit, mid-join seek after reboot.
- [x] **S5** Per-element `reveal_to()` (f84194b): one `.`/`-` per key-down element
      via an intra-char cursor; adds a per-symbol `TEL` debug line. HW-VALIDATED
      (`scripts/sync_reveal_test.py`): TEL stream == clue's 63-element Morse,
      intra-letter element gaps median 213 ms.
- [x] **S5-fix** Display-freeze regression (74ce5db): under a `resync()` seek the
      per-element reveal froze until the next render — the reveal clock was a
      free-running key-down counter that over/under-counted across a seek. Fixed by
      driving the reveal off `morse::Player::elems_elapsed()` (seek-safe, bounded
      by the total). Host unit test `scripts/morse_elems_test.sh`.

---

## Instructor-UI field exercise — receive-side fixes (FIELD-NOTES-20260623.md)

First live run of the on-device standalone Cardputer Instructor UI surfaced three
**receiving-side** firmware faults (the UI itself worked). Writeup +
root-cause analysis in `FIELD-NOTES-20260623.md`.

- [x] **§1 Heltec V3/V4 reboot-LOOP on first tone after startup.** ROOT-CAUSED +
      FIXED + HW-VALIDATED 2026-06-23 (commit 9a823d2). Reset reason captured over
      the V4's native USB: **`prev=9(BROWNOUT)`** — the full-volume first tone
      (level HIGH=32 ≈ 450 mA into 4 Ω on the shared 3V3 rail) sags the rail below
      the brownout threshold; the loop persists because mute is saved. **HW fix
      (validated): a single 100 µF electrolytic across the MAX98357A VDD→GND**, in
      parallel with the board's 10 µF + 0.1 µF, right at the amp (clean A/B), keeps
      full volume. **SW safety net (shipped):** brownout loop-breaker — boot muted
      for that boot only when `reset_reason()==BROWNOUT(9)` (`main.cpp`
      `g_boot_reset_reason`/`RST_BROWNOUT`). Soft-start amplitude ramp tried, did
      NOT fix the brownout (proving a current ceiling, not onset slew), kept only
      as harmless click-suppression.
- [x] **§1 follow-up: alert-at-MAX + unmute volume restore.** (commits fc35ac6 /
      1c01364.) (a) normal unmute now re-applies `config::volume()` in
      `apply_mute()` (was only clearing the mute flag); (b) the alert tone is forced
      to full level (`sidetone_set_level(32)`) for its duration and restores
      `config::volume()` on end (`start_alert_tone`/`alert_tone_tick`). With the
      100 µF cap fitted, a MAX alert tone is supply-safe. Note: alert at MAX does
      NOT re-arm the first-tone soft-start, so on an un-capped unit an idle→MAX
      alert is the worst-case brownout path (the loop-breaker still recovers it).
- [x] **§2 nRF52 (RAK/Wio) reboot on receiving mute.** Root cause was BLE-related,
      not the LittleFS-in-RX-path hypothesis: the nRF52 `ble_provision` stop()/begin()
      cycle re-runs `Bluefruit.begin()` against a live SoftDevice and hangs → 8 s
      watchdog reset. Mitigated 2026-06-23 (commit 9100640) by pinning
      `BLE_CYCLE_UNSAFE` (BLE_ON always); the proper fix (make stop()/begin() safe
      to cycle, then drop the pin) is tracked in
      [issue #3](https://github.com/benders/morse-station/issues/3).
- [x] **§3 Instructor locked out ~90 s after a broadcast command.** FIXED +
      HW-VALIDATED 2026-06-23 (commit 1c4ad62, stn73). Broadcast (255) relays can
      never finish early (no known ack count) so they waited the full
      `CTRL_BURST_WINDOW`=90 s, blocking the menu guard. Added `CTRL_BCAST_WINDOW`
      =6000 ms; `loop_instructor()` give-up check now selects the window by target.
      Menu frees in ~6 s.

---

## Multi-target port — Heltec V3 (Phase 1) + nRF52840 RAK4631 (Phase 2) + Wio Tracker L1 (Phase W)

Phased cross-MCU ports off the Heltec V4 baseline, all sharing one `src/` tree
behind `platform::`/`kv::`/`ble_provision::` seams introduced in **Phase 0**
(P0.1–P0.4: ESP32-only flags moved to `[esp32_base]`; `platform.h`/`kv.h` seams;
`config.cpp`/bootlog ring use `kv::Store`; both ESP32 envs build clean). Per-step
build logs are in git history + the plan docs; outcomes:

- [x] **Phase 1 — Heltec V3 (ESP32-S3 + SX1262, no FEM).** P1.1–P1.6. Added
      `[env:heltec_v3]`, a `DEVICE_HELTEC_V3` `pins.h` branch (no `HAS_FEM`,
      `BATT_GATE_ACTIVE_HIGH=0`), parameterised battery gate polarity, widened the
      device guards, and added V3 to the radio TCXO guard (1.8 V, same as V4).
      **HW-validated (station 38, 2026-06-11):** the one fix a real unit needed is
      that the V3's USB-C is a CP2102/UART0 bridge (not native USB), so the env
      overrides `-DARDUINO_USB_CDC_ON_BOOT=0`. Confirmed: serial + BLE, `pwr 0..3`,
      `txcw` at −0.53 ppm, watchdog, battery active-LOW gate. See
      `docs/components/heltec-v3.md`. (Later switched sidetone to I2S/MAX98357A,
      commit a0d249b.)
- [x] **Phase 2 — RAK4631 (nRF52840 + SX1262, no FEM).** P2.1–P2.13. Vendored
      `boards/wiscore_rak4631.json` + variant (from RAKWireless's
      `RAK-nRF52-Arduino`; provenance in `reference/rak4631/README.md`); added
      `[nrf52_base]` + `[env:rak4631]`; nRF52 implementations of `platform_`/`kv_`
      (LittleFS, one flat file per namespace)/`radio_` (global SPI, `SX126X_POWER_EN`,
      TCXO 1.8 V)/`ble_provision_` (Adafruit Bluefruit NUS)/`sidetone_` (silent
      stub)/`battery`/`display` (U8g2, no reset line, no VEXT gating). Build:
      **185,528 B flash (22.8%) / 27,636 B RAM (11.1%)**. **HW-validated on a real
      RAK4631 + RAK19007 + RAK1921** (flashed via `nrfutil`): LittleFS persistence,
      Bluefruit NUS, OLED, on-air RX + CW decode, runtime console. Two boot-hang
      bugs fixed in bring-up: (1) LittleFS never mounted — `kv_nrf52.cpp` called
      `InternalFS.open()` without `begin()` (fixed with `ensure_fs()`); (2) I2C
      OLED init hang — nRF52 TWIM `endTransmission` spin-waits with no timeout when
      a prior firmware left SDA low (fixed with `i2c_bus_recover()` + a `g_oled_ok`
      present-flag so a stuck/absent panel degrades to headless). `PIN_BUSY=46`
      confirmed against the datasheet. Remaining on-hardware work is tracked in
      [GitHub Issues](https://github.com/benders/morse-station/issues) (label `rak4631`).
- [x] **Phase W — Wio Tracker L1 Pro (nRF52840 + SX1262, no FEM, buzzer + SH1106).**
      W1–W9, plan + status in `wio-tracker-port.md`. Vendored
      `boards/seeed_wio_tracker_l1.json` + variant (from Meshtastic
      `seeed_wio_tracker_L1`, commit 8c4900a5; pin-map in
      `reference/wio-tracker-l1-pro/README.md`); `[env:wio_tracker_l1]`
      (`-DDEVICE_WIO_TRACKER_L1 -DSIDETONE_BUZZER`). Widened the nRF52 guards;
      Wio-only RF-switch via `setRfSwitchPins(SX126X_RXEN, SX126X_TXEN)` alongside
      `setDio2AsRfSwitch(true)`; a `-DSIDETONE_BUZZER` NRF_PWM0 backend (note: this
      variant's `g_ADigitalPinMap` is not identity — translate `PIN_BUZZER` D12 →
      abs GPIO 32 before `PSEL.OUT[0]`); active-HIGH battery gate; SH1106 (not
      SSD1306) display. **FULLY HW-VALIDATED 2026-06-07** (stn115). Required a
      SoftDevice fix first: the unit runs **S140 7.3.0** (not the 6.1.1 W1 pinned),
      so the app relinked for app-base 0x27000 (vendored `nrf52840_s140_v7.ld`),
      flashed via the UF2 bootloader. Confirmed: boot/reset-reason, LittleFS, OLED
      (SH1106 @ 0x3D), battery 4.18 V/99%, on-air RX, buzzer sidetone, buttons, BLE
      admin over NUS. Two HW bugs fixed: OLED right-edge distortion (SH1106 has a
      2-col offset, use `U8G2_SH1106_…`); constant buzzer "ticking" (Bluefruit auto
      conn-LED blinks LED_BLUE which aliases the buzzer pin → `autoConnLed(false)`).

---

## Stage 1 — Audio out (sidetone amp)

- [x] **Amp wired + working.** PAM8403 class-D board (Amazon B0DPMNYR2B) driving a
      4 Ω speaker (Amazon B0F3DBRXS5), powered from the Heltec 3V3/5V rail, common
      GND. GPIO4 → ~1 kΩ series + 100 nF → amp L_IN (GPIO4, not GPIO7 — GPIO7 is
      FEM power on the V4). See `docs/components/pam8403.md`. (Earlier MAX98357A
      I2S path + its first-tone brownout fix are archived above.)

## Stage 6/7 — Field testing

- [x] **Field-tested at range; power + message cadence tuned.** Range is good on
      **Low** in the open; operating rule (field note 1, 2026-06-19): open ground →
      Low/Med, fox indoors or across the whole camp → High. Low-power range across
      the camp area confirmed adequate for the hunt. (Operating rule: open ground →
      Low/Med, fox indoors or cross-camp → High; default the fox to Low so the
      volume/strength gradient stays usable, step up only for penetration/distance.)
- [x] **Enclosure + antenna build.** Hardware assembled.

---

## Stage 3 — Radio link (more)

- [x] **Heltec V4 FEM PA — engaged in TX modes (closed as-is).** `setup()` engages
      the FEM PA (`radio::set_pa(true)`) in the transmit run modes (Fox / Live Key)
      and bypasses it in Hunter (RX); the `pa` console command is a runtime
      override. LO/MED/HI/MAX labels are verbally approximate, so the same MAX
      radiates ~+28 dBm on the V4 (chip +22 + ~6 dB FEM PA) vs +22 dBm on the
      Wio / no-FEM boards (documented in `docs/protocol.md`). Accepted as-is — the
      long-term refinements (measure per-rev GC1109/KCT8103L PA gain, switch the PA
      per power level, surface chip-dBm→antenna-EIRP in `pwr`/`show`) are not being
      pursued. See `docs/components/heltec-v4.md`.

## RAK4631 — hardware bring-up (more)

- [x] **Fox-mode TX verified.** A hunter copies the RAK fox on-air; `setOutputPower`
      LO/MED/HI/MAX behaves (no FEM → +22 dBm ceiling).

## Cardputer ADV — Fox bring-up (more)

- [x] **On-air TX verified.** A Heltec hunter copies the Cardputer fox on
      905.0 MHz; `setOutputPower` LO/MED/HI/MAX checked (no FEM → +22 dBm ceiling),
      G0 tap cycles power.
- [x] **Keyboard text entry verified on hardware** — done as part of the standalone
      instructor-mode work: keymap/shift correctness, debounce feel, the opt-in
      window timing out cleanly, and edited values persisting + driving the fox
      loop. (The earlier intermittent typing crash could not be reproduced after
      the TCA8418 FIFO-drain bounding and was dropped.)
