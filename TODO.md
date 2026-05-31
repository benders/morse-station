# Morse Station — Work Plan

Incremental plan to get from "builds & flashes on Heltec V4" to a working
modified fox hunt: a stationary **fox** transmits a Morse description of its
location; **hunters** carry receivers that decode it by ear (and on the OLED).

Platform: **Heltec WiFi LoRa 32 V4** (ESP32-S3 + SX1262) for everything. The
Feather M0 / RFM69 path is dropped. We have **two Heltec V4 units** to test
with. The SX1262 does GFSK natively.

Legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs a decision

---

## Stage 0 — Done

- [x] Project builds and flashes to Heltec V4 (PlatformIO `heltec_v4` env).
- [x] RSSI band scanner running (current `src/main.cpp`) — gives us a picture
      of the 902–928 MHz noise floor to pick clear channels.

## Stage 1 — Audio out (sidetone)

Validate we can make sound before we worry about the radio.

- [~] Wire the amp: **PAM8403** class-D board (Amazon B0DPMNYR2B) driving a
      **4 Ω** speaker (Amazon B0F3DBRXS5). PAM8403 takes an analog audio input
      and runs off 2.5–5 V — power it from the Heltec 3V3/5V rail, common GND.
      Tentative pin: **GPIO 7** → ~1 kΩ series + 100 nF → amp L_IN. **Confirm
      GPIO 7 against the board silkscreen on hardware** (free of radio pins
      8–14, OLED 17/18/21, Vext 36).
- [x] Generate a 600 Hz sidetone on the MCU — `src/sidetone.cpp` uses the
      ESP32 LEDC peripheral (square wave; S3 has no true DAC). Builds clean.
      Verify clean tone on hardware and set volume with the amp's pot.
- [x] Wrap tone on/off behind `sidetone_on()` / `sidetone_off()`
      (`src/sidetone.h`) so the key handler and RX decoder can both drive it.
- [x] Flash `src/main.cpp` (Stage-1 beep test) and confirm audio on hardware.
      Confirmed: clean 600 Hz tone on GPIO 7 (cap not yet fitted, not needed).

## Stage 2 — Telegraph key input

- [x] Wire the key to a GPIO with `INPUT_PULLUP`, key shorts to GND
      (`PIN_KEY` in `src/pins.h`). Confirmed on **GPIO 6**: GPIO 5 read LOW
      even floating (pull-up wouldn't hold), GPIO 6 reads HIGH idle / LOW keyed.
- [x] Debounce — `src/morsekey.cpp` software debounce (5 ms settling). Add an
      RC cap across contacts too if edges are noisy. Confirm via serial.
- [x] Key down → `sidetone_on()`, key up → `sidetone_off()` (`src/main.cpp`
      Stage-2 local-keyer test). Verified on hardware: clean tone, no latency.

## Stage 3 — Radio link bring-up (FSK TX/RX on two units)

Needs a second Heltec V4 flashed as the counterpart.

- [x] Switchable TX/RX at runtime: hold PRG/BOOT button at boot = TX beacon,
      else RX listener (`src/main.cpp` Stage-3 test). One build, two units.
- [x] GFSK link on a single fixed channel: TX sends a 4-byte payload every 1 s,
      RX prints bytes + RSSI (`src/radio.cpp`, 905.0 MHz).
- [x] RadioLib SX1262 FSK config per design notes — 4.8 kbps, 5 kHz dev,
      sync word {0x2D,0xD4}, rxBw 39 kHz, low power (+2 dBm).
- [x] **V4 external FEM (PA+LNA) brought up** — `radio::fem_power_on()` powers
      VFEM (GPIO7), enables the FEM (CSD), sets PA-bypass, and `setDio2AsRfSwitch`
      lets DIO2 switch TX/RX on the V4.2. Without it RX was dead (~-107 dBm). One
      image drives the superset of V4.2 (GC1109) and V4.3.1 (KCT8103L) pins.
      See memory `heltec-v4-fem-rf-frontend`.
- [x] **V4.3 RX LNA fixed** — the KCT8103L CTX line is GPIO5 (`PIN_FEM_CTX`),
      software-driven (not DIO2), and doubles as the RX LNA select (LOW=LNA,
      HIGH=bypass). The old static-HIGH left V4.3 RX permanently LNA-bypassed →
      gauge read far below the V4.2. Now `fem_set_rx()`/`fem_set_tx()` track the
      radio state around `send()`/`start_receive()`. Confirmed on hardware: V4.3
      hunter gauge jumped to ~full, matching the V4.2 (fox on LO). Pin map from
      meshcore-dev/MeshCore PR #1867.
- [x] Bench link verified both directions, huge margin (two floors away ~-40
      dBm). Link is solid; see TX-gain note below.
- [ ] **Polish/range + compliance:** TX gain mismatched between revisions
      (V4.2 ~+34 dB hotter than V4.3 — different FEM TX path/PA mode under the
      superset config). Independent check: an SDR several floors away hears us
      at -80..-90 dBm with **no RX gain**, i.e. the V4.2 PA is clearly engaging
      and we are NOT in the +2 dBm low-power regime we configured. Range is
      plentiful for the camp either way. Before any deliberate outdoor deploy:
      equalise both boards to a matched power and decide the legal basis —
      §15.249 low-power unlicensed vs operating under an amateur license in the
      902-928 MHz (33 cm) band. Not blocking the hunt.

## Stage 3a — Field-tuning punch list

Round of tweaks from on-air testing — IDs, timing feel, display, power, build info.

- [x] Station ID fixups: ID for **21301 → 43**, ID for **21101 → 42**
      (provisioned into NVS via `scripts/provision.sh --port ... --id ...`).
- [x] Change sidetone from **600 Hz → 750 Hz** (`TONE_HZ` in `src/main.cpp`).
- [x] Slow keying to **15 WPM**; make WPM settable in NVS. `config::wpm()` (NVS
      key `wpm`, default 15, clamp 5..40); console `wpm <n>` / `provision.sh --wpm`.
- [x] Switch to **Farnsworth timing**; make the Farnsworth speed settable in NVS.
      `config::char_wpm()` (NVS key `char_wpm`, default 18, clamp wpm..40);
      `morse::Player::begin(wpm, char_wpm)` stretches the inter-char/word gaps via
      the ARRL formula (reduces to plain timing when char==overall). Console
      `farns <n>` / `provision.sh --farns`.
- [x] Increase the end-of-message delay before the next repeat begins. (REPEAT_PAUSE 3000 -> 7000 ms)
- [x] **Hunter decoder mis-reads the new slower / Farnsworth timing.** Fixed via
      option (a): the fox now broadcasts its `wpm`/`char_wpm` in the `Ident`
      packet (`src/protocol.h`), sent at the top of each message loop (plus the
      8-min legal cadence). `morse::Decoder::begin(wpm, char_wpm)` derives
      Farnsworth-aware thresholds (dit/dah, char-gap, word-gap) from a shared
      `farns_gaps()` helper, and the hunter retunes its decoder + watchdog when
      it hears an Ident (`loop_hunter` in `src/main.cpp`). Reduces to the old
      2u/5u thresholds for plain timing. Live-keying still uses local config
      speeds (no Ident) — fine until live-key is back in scope.
- [x] Hunter display: show only the most recent characters, scrolling off as new
      ones arrive. Single copy line in `display::hunter` shows the tail of the
      rolling buffer (last 21 glyphs — the full 128px width at 6px; see the
      per-element / freeze-fix notes in Stage 6).
- [x] Hunter display: show the **station ID of the TX node** plus the operating
      frequency. Header shows `RECV <id>` (the `station_id` from the last received
      `KeyState`/`Ident` in `loop_hunter`, so it populates on first signal, not
      just on Ident; `RECV ---` until anything is heard) plus
      `radio::frequency_mhz()` (905.0 MHz) right-justified. (Earlier revisions
      showed the TX callsign as `HUNTER <CALL>`; a numeric ID is terser for DF.)
- [x] Fox default power on boot → **Low** (`pwr_idx = 0` in `src/main.cpp`).
- [x] Add a **MAX** power level to the fox — transmit at full **28 dBm**. Added
      `{"MAX", 22}` to `PWR_LEVELS` (4th level); +22 dBm is the SX1262 chip
      ceiling, ~28 dBm at the antenna after the FEM PA. "PWR MAX" fits the OLED.
- [x] On bootup, show the **git revision** of the build — `scripts/git_rev.py`
      injects `-DGIT_REV`; shown on the OLED splash for 2 s in `setup()`
      (`display::status`) and also printed to serial.
- [x] **Persist the fox TX power setting in NVS.** `config::fox_pwr_idx` /
      `set_fox_pwr_idx` (NVS key `fox_pwr_idx`, default 0=LO) store the index;
      restored into `pwr_idx` on boot (clamped) and saved on each PRG tap.
- [x] **Hunter dit/dah display mode.** PRG/BOOT tap in hunter mode toggles the
      copy line between decoded letters (default) and raw dit/dah elements. The
      decoder stashes each completed char's elements (`morse::Decoder::
      last_elements()`); `loop_hunter` mirrors them into `ditdah_buf`.

## Stage 4 — Fox message loop (pre-programmed transmit)

- [x] Fox message stored in firmware (`FOX_MSG` in `src/main.cpp`).
- [x] Text → Morse timing: `morse::Player` (`src/morse.cpp`) maps chars to
      dit/dah sequences with standard WPM timing (1/3/1/3/7-unit gaps), as a
      non-blocking timed key-state stream so it can feed sidetone *and* radio.
- [x] Local sidetone driven from the Morse timing (Stage-4 `main.cpp`).
      Verify timing by ear on hardware.
- [x] Loops with a configurable `REPEAT_PAUSE`.

Also added (used by RX in Stage 5/6): `morse::Decoder` — key timing → text.

## Stage 5 — Morse over FSK — keystate broadcast

**Decided: keystate broadcast (design-notes approach A).** Students will want
to live-key to each other after the hunt, and only this scheme carries live
keying with real timing. The canned fox loop just feeds the same keystate
stream from the encoder instead of a human key, so one protocol covers both
uses.

- [x] TX sends a 5-byte `KeyState` packet every 30 ms (magic, station id,
      key state, seq). RX reconstructs timing and drives its sidetone. No
      ACKs/retries. (`src/protocol.h`, Stage-5 `src/main.cpp`.)
- [x] Fox loop feeds the keystate stream from the `morse::Player` — same wire
      format the RX uses for a live key.
- [x] RX side: key stream → sidetone + `morse::Decoder` → text (serial now;
      OLED in Stage 6).
- [x] End-to-end builds: fox → transmit → hunter hears Morse + decodes text.
      Verify on two units; OLED display is Stage 6.

## Stage 6 — Fox-hunt integration

- [x] Fox mode: loops the message, transmits keystate, OLED "FOX TX" status.
- [x] Hunter mode: receives, plays sidetone, OLED decoded text + RSSI bar
      (warmer/colder hint). (`src/display.cpp`, Stage-6 `src/main.cpp`.)
- [x] Live-key mode added (students key to each other after the hunt).
- [x] Mode select at boot — PRG-button menu (short = cycle, long = select,
      8 s auto-select).
- [x] Hunter audio volume tracks RSSI (analog "tune for max volume" feel) —
      received RSSI maps over the same -110..-40 dBm span as the bar onto the
      sidetone LEDC duty cycle. Curve is **exponential (dB-linear)** so equal
      RSSI steps give equal perceived loudness steps; a linear map was
      inaudible (all the change sat in the flat top of the square wave's
      sin(pi*duty) response). Near-silent floor (duty 3). **Verified on
      hardware.** Note: the gradient only appears when RSSI actually varies, so
      run the fox at LO power — at MED/HI the strong link saturates RSSI across
      the search area. (`sidetone_set_volume` in `src/sidetone.cpp`,
      `loop_hunter` in `src/main.cpp`.)
- [x] Fox adjustable TX power — short PRG tap cycles LO(-9)/MED(+2)/HI(+14) dBm
      via `radio::set_tx_power` (SX1262 output; FEM PA follows). Shown as
      "PWR x" on the fox OLED, boots MED. Lets us pull power down in a small
      space. **Verified on hardware.** (`src/main.cpp`, `src/radio.cpp`,
      `src/display.cpp`.)
- [x] Hibernate (power-off stand-in, no hardware switch) — boot-menu item
      powers down the FEM + peripheral rail and enters deep sleep with no wake
      source; RST restarts the sketch into the menu. **Verified on hardware.**
      (`hibernate()` in `src/main.cpp`.)
- [x] **RX stuck-key on signal loss:** when an RX node lost the signal mid-key
      it could latch in "key active" (sidetone stuck on / decoder hung). Fixed
      with a watchdog in `loop_hunter` — no keystate packet for ~one dah
      (`RX_TIMEOUT_MS`, 3 units at WPM) forces key-up → sidetone off → idle.
      **Verified on hardware.**
- [ ] Field test at range; tune power and message cadence (hardware).
- [ ] "RECV" station ID and signal strength bar should clear after timeout from
      last received packet.
- [ ] When receiving from one Fox (not timed out), packets from other stations
      must be ignored.
- [x] "dit dah" display scrolling is now **per element** (each `.`/`-` scrolls as
      the decoder classifies it on the key-up edge, via `morse::Decoder::
      take_element()`), and dit/dah is the **default** view (PRG tap still toggles
      to letters). Letters separated by a space, words by `" / "`. (`loop_hunter`,
      `src/main.cpp`.)
- [x] "text" scrolling freeze fixed — the rolling copy buffer's "drop oldest half"
      had an off-by-one that dragged the NUL terminator into mid-buffer once the
      buffer first filled (~127 chars), severing the string so `strlen` froze and
      the display stopped advancing. Trim now moves relative to `len` and
      re-terminates; both buffers share one `rolling_append()` helper. Copy line
      also widened from 16 → **21 glyphs** (full 128px width). **Verified on HW**
      (decode-timing capture confirmed steady per-char advance past the wrap point).


## Stage 7 — Range & polish

Start at **low power, single fixed channel, no hopping** (§15.249, ~200–400 m)
— expected to be enough for a camp. FHSS is postponed to a later phase.

- [ ] Field-test low-power range across the camp area.
- [ ] **Later phase, only if range falls short:** FHSS — 50-channel hop table
      + RX scan-and-lock (`rx-sync.md`, a TODO in design notes) for §15.247
      full-power operation.
- [x] Per-unit station IDs in NVS — `src/config.cpp` (random id on first
      boot, override via `set_station_id()`).
- [x] Default station ID derived from the eFuse MAC (`ESP.getEfuseMac()`, 6
      bytes XOR-folded into 1..254) — stable per unit, no stored value needed.
      `set_station_id()` override still persists in NVS. (`src/config.cpp`,
      ref espressif/arduino-esp32#932.)
- [x] Callsign + fox message in NVS (`src/config.cpp`), provisioned over serial:
      boot setup console (`run_setup_console` in `src/main.cpp`, commands
      `call`/`msg`/`id`/`show`/`done`) plus a host-side helper
      `scripts/provision.sh` (--call/--msg/--id/--show). **Verified on hardware:**
      provisioned both units to KC8HOB and confirmed in the fox loop. DTR/RTS
      auto-reset is best-effort; pass `--port` and tap RST if the window is missed.
- [ ] **Field provisioning via BLE** (preferred over serial for the field).
      Stand up a BLE GATT config service on the S3 (it has BLE on-silicon) so a
      phone — or a "master" unit — can set callsign/fox-message/station-id
      without a laptop + USB. Serial console stays as the bench path. Considered
      USB-OTG host (S3 supports it, full-speed) but rejected: it needs VBUS
      sourcing and OTG-wired connectors the V4 doesn't provide, and ties up the
      programming port. ESP-NOW broadcast is a lighter fallback if BLE proves
      fiddly.
- [ ] Callsign ID: fox sends a periodic station-ID packet (`proto::Ident`, 8-min
      cadence, `tx_ident` in `src/main.cpp`) for Part 97 operation; the callsign
      also rides in the fox message text for an audible CW ID. Still missing an
      end-of-communication ID (no clean fox-exit path today). Only relevant if
      run under an amateur license — see Stage 3 compliance note and
      `design-notes.md`.
- [ ] Enclosure, antenna build (hardware).

---

## Cardputer ADV port (experimental fox)

Port the firmware to the **M5Stack Cardputer ADV** (Stamp-S3A / ESP32-S3) to use
as an experimental **fox** with onboard sound, a keyboard, and a colour LCD. The
two platforms share one `src/` tree, selected by `-DDEVICE_HELTEC_V4` /
`-DDEVICE_CARDPUTER_ADV` (PlatformIO env). Device-specific code lives behind that
define or in `*_cardputer.cpp` files whose whole body is `#ifdef
DEVICE_CARDPUTER_ADV` (so only one display/sidetone implementation compiles per
env). The Heltec build is untouched.

### Hardware facts (researched, confirm on HW)

- **Radio is NOT onboard.** It rides on the removable **Cap LoRa-1262** (bare
  **SX1262** + ATGM336H GNSS, RP-SMA antenna, **no external PA/FEM**).
  - **Max TX power = +22 dBm** (SX1262 chip ceiling). vs the Heltec V4's
    ~+27–28 dBm through its FEM. `PWR_LEVELS` already tops out at `MAX`=+22, so
    the table maps directly — there's just no FEM gain on top, and **no FEM
    bring-up** (simpler than V4). The cap is installed.
  - Pins (M5 docs): NSS=G5, DIO1=G4, RST=G3, BUSY=G6, SCK=G40, MOSI=G14,
    MISO=G39. SPI bus is shared with the microSD slot. `setDio2AsRfSwitch(true)`
    drives the on-cap TX/RX switch (same as the Heltec).
  - **[x] TCXO = 1.8 V confirmed on HW** — `beginFSK()` succeeds and the cap
    receives real off-air Morse, so the 1.8 V TCXO setting in `radio.cpp` is
    correct (not a plain crystal).
- **Audio = ES8311 codec + NS4150B amp** (I2S, no PWM-to-pin shortcut). Driven by
  **M5Unified `M5.Speaker`**. I2S pins (FYI, M5 owns them): SCLK=G41, LRCK=G43,
  DSDIN=G42, ASDOUT=G46 (no MCLK).
- **Display = ST7789V2** 240x135 colour. BL=G38, RST=G33, DC=G34, MOSI=G35,
  SCK=G36, CS=G37. Driven by **M5.Display (M5GFX)**.
- **Keyboard = TCA8418** I2C keypad controller @ **0x34** on the internal I2C bus
  (SDA=G8, SCL=G9), **INT=G11** (active-low, falling edge). Bus is shared with
  the ES8311 (0x18) and BMI270 IMU (0x68).
- Button G0 (BtnA/BOOT) reused as the boot-menu / power-cycle button.

### Decisions

- **[x] M5Unified for audio.** `M5.Speaker` drives the ES8311 sidetone
  (`sidetone_cardputer.cpp`). `M5.begin()` also claims the LCD, so the display
  uses **`M5.Display` (M5GFX, same LovyanGFX family)** rather than a standalone
  LovyanGFX instance — both funnel through `cardputer_m5_begin()`
  (`platform_cardputer.{h,cpp}`). LovyanGFX dropped from the env's `lib_deps`
  (M5GFX comes with M5Unified).

### Stage C0 — Build + groundwork (firmware, no HW yet)

- [x] Device-split `pins.h` (Heltec / Cardputer blocks, `#error` if neither).
- [x] `radio.cpp` device-aware: FEM guarded by `HAS_FEM` (Heltec only); Cap pins
      + DIO2 RF switch for the Cardputer.
- [x] Sidetone: existing LEDC path guarded to Heltec; new `sidetone_cardputer.cpp`
      (M5.Speaker), same `sidetone.{on,off,set_volume,init}` API.
- [x] Display: existing U8g2 path guarded to Heltec; new `display_cardputer.cpp`
      reimplementing the `display::` API on the 240x135 panel via M5.Display.
- [x] `main.cpp` hibernate FEM/Vext pins guarded by `HAS_FEM`.
- [x] platformio `cardputer_adv` env: RadioLib + M5Unified.
- [ ] **`pio run -e cardputer_adv` builds clean.** Resolve any M5Unified API
      drift (Speaker `tone`/`stop` signatures, `TFT_*` colour macros).

### Stage C1 — Fox bring-up on hardware (existing functionality)

Goal: the Cardputer runs the **fox** exactly like the Heltec — message in NVS,
set via the **serial console**, simple LCD status, keystate TX on the cap.

- [x] Flash + boot OK: `M5.begin()` (LCD + ES8311) comes up with no crash; boots
      through splash + menu (auto-select). `mode=N station_id=73` printed.
- [x] **Radio init succeeds** — `beginFSK()` returns OK both as hunter and fox;
      as a hunter the cap decoded real off-air Morse ("IS K"), proving SPI pins +
      TCXO + sync + frequency. Serial-provisioned `mode 1` boots straight to fox.
- [x] Serial provisioning REPL works over USB-CDC; added `mode <0..2>` to set the
      default boot mode without the G0 button. `call`/`msg`/`mode` persisted and
      confirmed via `show` across reboot.
- [ ] **Sidetone (needs an ear):** confirm a clean keyed tone from the onboard
      speaker / 3.5 mm jack via M5.Speaker at the fox WPM; volume sane.
- [ ] **On-air TX (needs a hunter):** confirm a Heltec hunter copies the
      Cardputer fox on 905.0 MHz. Check `setOutputPower` LO/MED/HI/MAX (no FEM →
      MAX +22 dBm is the real ceiling). G0 tap should cycle power.
- [x] Sound + text confirmed good on HW (M5.Speaker sidetone + fox message
      render). Fox loop runs as KC8HOB.
- [x] **Display flicker fixed** — the panel was cleared+redrawn directly each
      ~10 Hz frame. `display_cardputer.cpp` now renders into a full-frame
      `M5Canvas` back buffer and `pushSprite`s it once per frame. Stable on HW:
      ran ~100 s, seq→2727, free heap flat at 275 KB (no leak), zero reboots. A
      brief early "reboot loop" did not reproduce. Boot now logs its reset reason
      to NVS (`# boot #N reason now=.. prev=..`) so any recurrence is diagnosable.
- [x] **Fox runs silent** — local sidetone disabled in fox mode (a beeping
      transmitter is easy to find by ear). Hunters still copy it from the
      received keystate. Applies to both platforms (`loop_fox` in `main.cpp`).
- [ ] Fox `setOutputPower` LO/MED/HI/MAX + G0 power-cycle; Ident cadence; LCD "*".

### Stage C2 — Keyboard entry (Cardputer-native config)

Goal: set the **fox message and callsign from the keyboard**, no laptop.

- [x] TCA8418 driver (`keyboard_cardputer.cpp`): register init (matrix 7x8, event
      mode, falling-edge int enables) over **M5.In_I2C** (shares M5's bus on
      G8/G9 — no second I2C master), polled event FIFO, keynum→(row,col) decode,
      and the 4x14 keymap with shift/capslock. Adapted from the M5Cardputer-ADV
      reference HAL. Returns ASCII + `\b`/`\r`/`\t`. INT=G11 parked as input
      (we poll the event count, not the line).
- [x] On-screen text-entry editor (`config_ui_cardputer.cpp`): field editor
      (append / backspace / ENTER-commit) on the LCD, reused for callsign and
      message.
- [x] Boot hook: `config_ui::run()` after the splash (Cardputer only) — a 2 s
      "press any key" window opens a 1=Callsign / 2=Message menu; writes go
      through `config::set_callsign` / `set_fox_message` (same NVS keys as the
      serial console). Builds clean.
- [ ] **Verify on hardware:** keymap/shift correctness (esp. symbols), debounce
      feel, that the opt-in window times out cleanly, and that edited values
      persist + drive the fox loop.
- [ ] **Intermittent crash while typing a message on the keyboard**, followed by
      several reboots (seen once on HW 2026-05-31, did not reproduce on a second
      try). No serial capture of the panic yet — the USB port re-enumerates on the
      reboot so a plain `cat` of the port dies; capture with a reconnecting loop
      and watch for the ESP32 backtrace next time. Suspects: TCA8418 FIFO handling
      / keynum decode in `keyboard_cardputer.cpp`, or the editor buffer in
      `config_ui_cardputer.cpp` (bounds on append?). The `# boot #N reason` NVS log
      should record the reset reason — check it after a recurrence.
- [ ] Decide whether keyboard config also covers wpm/farns/id, or those stay
      serial-only (currently serial-only).

### Stage C3 — Polish (later)

- [ ] Confirm legal power basis at +22 dBm (no FEM) — see Stage 3 compliance note.
- [ ] Optional: use the cap's ATGM336H GNSS to put the fox's grid/coordinates in
      the message automatically.
- [ ] Battery/run-time check (1750 mAh onboard) for a fox left running.
- [ ] Live-key mode: needs a physical key on a free GPIO (Grove G1/G2 — confirm);
      out of scope for the experimental fox.
