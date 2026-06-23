# Morse Station — Completed Work

Archive of finished items, moved out of `TODO.md`. The fox-hunt firmware: a
stationary **fox** transmits a Morse description of its location; **hunters**
decode it by ear and on the display. Primary platform **Heltec WiFi LoRa 32 V4**
(ESP32-S3 + SX1262); the **M5Stack Cardputer ADV** is an experimental second fox
sharing one `src/` tree. See `docs/` for the protocol, command reference, and
per-component notes; open items remain in `TODO.md`.

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
