# Morse Station — Work Plan

Remaining work toward a working modified fox hunt: a stationary **fox** transmits
a Morse description of its location; **hunters** carry receivers that decode it by
ear (and on the display).

Platform: **Heltec WiFi LoRa 32 V4** (ESP32-S3 + SX1262) as the primary unit, with
the **M5Stack Cardputer ADV** as an experimental second fox sharing one `src/`
tree. Starting at low power on a single channel — **FHSS postponed**.

Completed items have moved to **`DONE.md`**. Reference material lives in `docs/`:
- `docs/protocol.md` — over-the-air protocol + parameters (and the regulatory basis / FHSS plan).
- `docs/commands.md` — the serial / BLE command reference.
- `docs/components/` — per-component lessons (Heltec V4, Cardputer ADV, SX1262, PAM8403).

Legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[?]` needs a decision

---

## Multi-target port — Phase 2 (RAK4631 / nRF52840 cross-MCU port)

- [x] **P2.1** Vendored `boards/wiscore_rak4631.json` (verbatim from the plan)
      and the variant: cloned `RAKWireless/RAK-nRF52-Arduino` (the upstream
      source for `WisCore_RAK4631_Board`) and copied
      `variants/WisCore_RAK4631_Board/{variant.h,variant.cpp}` into
      `variants/`, with `board_build.variants_dir = variants` in
      `[nrf52_base]`. **Variant strategy: Option A (vendored), sourced from
      Option B's fork** — see `reference/rak4631/README.md` for the full
      provenance writeup and why we didn't pin `platform_packages` to the
      whole fork. Confirmed Wire SDA=13/SCL=14 against the vendored variant.h;
      BUSY 46-vs-39 and the VBAT divider ratio remain hardware TODOs (flagged
      in the README and in `pins.h`/`battery.cpp` comments).
- [x] **P2.2** `platformio.ini`: added `[nrf52_base]` anchor (platform
      nordicnrf52, framework arduino, variants_dir) and `[env:rak4631]`
      (board `wiscore_rak4631`, RadioLib + U8g2 lib_deps, `-DDEVICE_RAK4631`,
      the `-DPIN_*` radio flags + `-DSX126X_POWER_EN=37`, `upload_protocol =
      nrfutil`). Dropped the planned `RADIOLIB_EXCLUDE_*` flash-shrink flags —
      RadioLib 7.7.x's inter-module type aliases break with a partial exclude
      set, and the build fits comfortably without them (see P2.12 sizes).
- [x] **P2.3** `src/pins.h`: added `#elif defined(DEVICE_RAK4631)` branch
      consuming the radio `-DPIN_*` / `SX126X_POWER_EN` flags via `#ifndef`
      fallbacks, no `HAS_FEM`/`PIN_FEM_*`, `PIN_OLED_RST` sentinel (display.cpp
      uses `U8X8_PIN_NONE` directly), `PIN_VBAT_READ=5`, `PIN_SIDETONE=-1`, and
      placeholder `PIN_KEY`/`PIN_MODE_BTN` marked CONFIRM-ON-HARDWARE.
- [x] **P2.4** `src/platform_nrf52.cpp`: `restart()`→`NVIC_SystemReset()`,
      `system_off()`→`sd_power_system_off()` (+ raw-register fallback),
      `reset_reason()` reads/clears `NRF_POWER->RESETREAS` and folds the
      bitfield to a small linear enum (priority-ordered), `unique_id_byte()`
      XOR-folds `FICR->DEVICEID[0]^[1]`. Moved `reset_reason_label()` behind
      `platform::` (added to `platform.h`, ESP table moved into
      `platform_esp32.cpp`, nRF52 table added here); `main.cpp` now calls
      `platform::reset_reason_label()`.
- [x] **P2.5** `src/kv_nrf52.cpp`: `kv::Store` over `Adafruit_LittleFS` +
      `InternalFileSystem` — one flat file per namespace (`/<ns>.kv`) holding a
      small serialized key→value map (type-tagged entries), loaded on
      `begin()`, rewritten atomically (temp file + rename) on every mutation.
      Same get/put/isKey/remove semantics as the ESP wrapper.
- [x] **P2.6** `src/radio.cpp`: nRF52 branch uses the global `SPI` (re-pinned
      via `SPI.setPins`/`begin()`) instead of `HSPI`, neutralizes `IRAM_ATTR`
      (empty macro on nRF52), drives `SX126X_POWER_EN` HIGH + 10 ms settle
      before `beginFSK()`, and adds a `DEVICE_RAK4631` TCXO_V=1.8f case. No FEM
      branch (HAS_FEM stays undefined, same as Heltec V3).
- [x] **P2.7** `src/ble_provision_nrf52.cpp`: implements `ble_provision::` via
      Adafruit Bluefruit `BLEUart` — `begin()` brings up Bluefruit + advertises
      the NUS service, `process()` polls `bleuart.available()`/`read()` into
      the same line-assembler/dispatch shape as the NimBLE path (no FreeRTOS
      queue needed — single-threaded on the main loop), `BleOut` chunks writes
      to 20 B like the ESP32 `BleOut`. Guarded `ble_provision.cpp` (NimBLE) to
      `#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) ||
      defined(DEVICE_CARDPUTER_ADV)`.
- [x] **P2.8** `src/sidetone_nrf52.cpp`: silent stub — every `sidetone.h`
      symbol is a no-op but stores mute/level/volume state so `show` stays
      sane; `// TODO: tone()-based piezo path`. `sidetone.cpp` (I2S) was
      already correctly guarded to `#if defined(DEVICE_HELTEC_V4) ||
      defined(DEVICE_HELTEC_V3)` from Phase 1.
- [x] **P2.9** `src/battery.cpp`: added `#elif defined(DEVICE_RAK4631)` branch
      — `analogRead(PIN_VBAT_READ)` through the RAK19007 divider (placeholder
      ratio `DIVIDER_RAK=2.0f`, internal ADC reference; **TODO CALIBRATE**
      against the schematic / a meter on real hardware) → `volts_to_percent()`
      (reused unchanged); `charging()` best-effort `false`.
- [x] **P2.10** `src/display.cpp`: extended the U8g2 guard to `#if
      defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) ||
      defined(DEVICE_RAK4631)`; added a RAK-specific
      `U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE)` constructor
      (no reset line, default Wire) and made `begin()` skip the VEXT rail
      gating for RAK (RAK19007 has no switched peripheral rail).
- [x] **P2.11** `main.cpp`: confirmed it fits unchanged — the
      `#ifdef DEVICE_CARDPUTER_ADV` blocks and `#ifdef HAS_FEM` hibernate path
      compile out cleanly; `platform::system_off()`/`platform::restart()`
      already cover the nRF52 sleep/reset paths from Phase 0. Only change
      needed was routing `reset_reason_label()` through `platform::` (P2.4).
- [x] **P2.12** `pio run -e rak4631` **links clean and fits comfortably**:
      **185,528 B flash (22.8% of 815,104) / 27,636 B RAM (11.1% of 248,832)**.
      Regression builds all still pass and match baselines:
      `heltec_v4` 625,785 B / 34,684 B, `heltec_v3` 625,525 B / 34,748 B,
      `cardputer_adv` 761,173 B / 34,920 B (the few-byte deltas vs. the stated
      baselines are the new `platform::reset_reason_label()` indirection).
      (Clean build now 186,048 B flash after the two boot fixes below.)
- [x] **P2.13** **Hardware-validated on a real RAK4631 + RAK19007 + RAK1921.**
      Flashed via `nrfutil` (1200bps DFU). Confirmed working: LittleFS
      persistence (`show` config + `bootlog` ring round-trip across reboots),
      Bluefruit NUS advertising (`MorseStn-26`), OLED (scrolling copy + live
      RSSI bar), radio RX + CW decode, runtime serial console. Two boot-hang
      bugs found and fixed during bring-up:
        1. **LittleFS never mounted** — `kv_nrf52.cpp` called `InternalFS.open()`
           without `InternalFS.begin()`; first kv access (bootlog) hit
           `assert(block < lfs->cfg->block_count)` → `abort()` → silent hang.
           Fixed with a one-time `ensure_fs()` in `kv::Store::begin()`.
        2. **I2C OLED init hang** — nRF52 TWIM `endTransmission` spin-waits on
           `EVENTS_TXSTARTED` with no timeout; the OLED left mid-byte by the
           prior firmware held SDA low across reset, so START never issued.
           Fixed with `i2c_bus_recover()` (9-clock + STOP) + a `g_oled_ok`
           present-flag that no-ops all draws so a stuck/absent panel degrades
           to headless instead of bricking the node. `PIN_BUSY=46` confirmed
           correct against the RAK4631 datasheet SX1262 table (P1.14).
      Remaining on-hardware work is tracked in **RAK4631 — remaining hardware
      TODOs** below.

---

## RAK4631 — remaining hardware TODOs

The nRF52840 port is hardware-validated (implementation/build log: Phase 2
P2.1–P2.13 above). These items still need a RAK4631 on the bench:

- [ ] **VBAT divider calibration.** `DIVIDER_RAK=2.0f` in `src/battery.cpp` is a
      placeholder that currently reads ~5.0 V on USB (wrong ratio). Measure the
      real cell voltage with a meter on battery power and set the correct
      RAK19007 divider ratio.
- [ ] **Confirm `PIN_KEY` / `PIN_MODE_BTN` GPIOs.** Both are placeholders in
      `src/pins.h` marked CONFIRM-ON-HARDWARE; the boot menu currently reaches
      modes via the 5 s idle auto-select rather than a button press. Verify and
      assign the real GPIOs (and confirm the menu short-press/long-press flow).
- [ ] **Fox-mode TX verification.** RX + CW decode is proven on-air; TX has not
      been keyed yet. Confirm a hunter copies the RAK fox and that
      `setOutputPower` LO/MED/HI/MAX behaves (no FEM → +22 dBm is the ceiling).

---

## Wio Tracker L1 Pro port (Phase W — see wio-tracker-port.md)

Sibling nRF52840+SX1262 port, direct precedent = the RAK4631 above. Plan +
status board live in `wio-tracker-port.md`.

- [x] **W1** Vendored `boards/seeed_wio_tracker_l1.json` +
      `variants/Seeed_Wio_Tracker_L1/{variant.h,variant.cpp}` (from upstream
      Meshtastic `meshtastic/firmware` commit `8c4900a5`, board
      `seeed_wio_tracker_L1` — no L1-Pro-specific variant exists upstream; the
      plain L1's OLED variant is the right base, see
      `reference/wio-tracker-l1-pro/README.md` for full provenance + the
      resolved pin-map table). Added `[env:wio_tracker_l1]` to `platformio.ini`
      (extends `nrf52_base`, `-DDEVICE_WIO_TRACKER_L1 -DSIDETONE_BUZZER`,
      RadioLib + U8g2, `upload_protocol = nrfutil`). `pio run -e
      wio_tracker_l1` resolves the board+variant and fails only in
      `radio.cpp`'s still-RAK4631-only branches (HSPI/IRAM_ATTR/TCXO_V/
      SPI.begin 4-arg) — exactly the expected W1 outcome; `rak4631` still
      builds clean.
- [x] **W2** `src/pins.h` — `DEVICE_WIO_TRACKER_L1` branch. Mirrors the
      env's `-DPIN_*` (Arduino logical numbers); no `HAS_FEM`/`PIN_FEM_*`,
      no `SX126X_POWER_EN` (always-on LoRa LDO); `PIN_OLED_RST=-1` sentinel;
      `PIN_BUZZER`→`PIN_SIDETONE`; `PIN_KEY`=Menu_Key, `PIN_MODE_BTN`=
      Joystick_Press (no Rot_Key on this board); `BATT_GATE_ACTIVE_HIGH 1`.
      `pio run -e wio_tracker_l1` now fails in `radio.cpp` (HSPI/TCXO_V/
      on_rx/SPI.begin 4-arg — W3 territory), not pins.h; `rak4631` still
      links clean (12.3% RAM / 23.4% flash).
- [x] **W3** `src/radio.cpp` — widened RAK4631 guards (IRAM_ATTR shim,
      Module-over-global-SPI, TCXO_V=1.8f) to also cover
      `DEVICE_WIO_TRACKER_L1`; kept `SX126X_POWER_EN` RAK-only (Wio has no
      power-enable pin). Added Wio-only RF-switch handling: kept
      `setDio2AsRfSwitch(true)` and additionally called
      `chip.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN)` (consuming the variant's
      own macros — `SX126X_RXEN`=D5/`LoRa_SW`, `SX126X_TXEN`=`RADIOLIB_NC`),
      matching upstream Meshtastic's `SX126xInterface.cpp` precedent for boards
      combining `SX126X_DIO2_AS_RF_SWITCH` with a discrete RXEN/TXEN pair.
      `radio.cpp.o` now compiles clean for `wio_tracker_l1` (failure moved on
      to `flash_cache.c`/`LED_BUILTIN`, an unported-TU issue for later phases);
      `rak4631` still links clean (191100 flash / 30492 RAM, byte-identical to
      pre-W3); `heltec_v4` still configures+links. RXEN/TXEN wiring is
      hardware-unvalidated — needs an on-air RX check in W9.
- [x] **W4** `src/sidetone_nrf52.cpp` — new `-DSIDETONE_BUZZER` backend
      (NRF_PWM0 square wave). NOTE: this variant's `g_ADigitalPinMap` is **not**
      identity (unlike RAK) — must translate `PIN_BUZZER` (logical D12) to
      absolute GPIO 32 before writing `PSEL.OUT[0]`; see the README's pin-map
      section.
- [x] **W5** Buttons — `PIN_KEY = Menu_Key` (logical D13/abs GPIO8, the
      variant's `CANCEL_BUTTON_PIN`/User Button); no `Rot_Key` exists on this
      board, substitute `Joystick_Press` (`TB_PRESS`, logical D29/abs GPIO37)
      for `PIN_MODE_BTN`. No code needed — satisfied by the W2 pin defines;
      `main.cpp`/`MorseKey` handling is device-agnostic (INPUT_PULLUP, LOW=
      pressed). Verified against variant.h; no pin collisions.
- [x] **W6** `src/battery.cpp` — gated read; gate (`BAT_CTL`, logical D30/abs
      GPIO4) is **active-HIGH** per upstream `initVariant()` (opposite the
      RAK's always-on divider); `ADC_MULTIPLIER = 2.0` upstream — confirm ratio
      on the bench.
- [x] **W7** `src/display.cpp` — include Wio in the U8g2 SSD1306 I2C path.
- [x] **W8** Extend `platform_nrf52`/`kv_nrf52`/`ble_provision_nrf52`/
      `sidetone` device guards to cover `DEVICE_WIO_TRACKER_L1`; also added
      `#define LED_BUILTIN PIN_LED1` to the vendored variant.h (InternalFS
      needs it; NOT LED_BLUE — that's the buzzer pin). `wio_tracker_l1` now
      LINKS clean (RAM 27748/248832, Flash 186528/815104).
- [x] **W9 — FULLY HARDWARE-VALIDATED (2026-06-07).** Required a SoftDevice fix
      first: the unit runs **S140 7.3.0** (not the 6.1.1 W1 pinned), so the app
      had to relink for app-base 0x27000 (vendored `nrf52840_s140_v7.ld` +
      `board_build.ldscript`). Flashed via the UF2 bootloader (serial nrfutil DFU
      was flaky). Confirmed on hardware: boot banner + reset-reason, LittleFS
      config, OLED (SH1106) @ 0x3D, battery 4.18V/99%, **on-air RX**, **sidetone**
      (buzzer), **buttons**, and **BLE admin** over NUS (`show`/`bootlog`/`batt`/
      `help` + a `wpm` write+persist round-trip via `scripts/ble_cmd.py 115`).
      Two bugs found on hardware and fixed:
        - [x] OLED slightly distorted on the right edge — the panel is an
          **SH1106** (132-col RAM, 2-col visible offset), not an SSD1306.
          Fixed by using `U8G2_SH1106_128X64_NONAME_F_HW_I2C` for the Wio in
          `display.cpp` (RAK keeps SSD1306). Confirmed clean on hardware.
          (The variant's `USE_SSD1306` flag was not authoritative.)
        - [x] Constant buzzer "ticking" — Bluefruit auto conn-LED blinks
          LED_BLUE, which aliases the buzzer pin (D12/P1.00). Fixed:
          `Bluefruit.autoConnLed(false)` in `ble_provision_nrf52.cpp` (Wio-only
          guard). Confirmed silent on hardware.

---

## Multi-target port — Phase 1 (Heltec V3 build target)

- [x] **P1.1** `platformio.ini`: added `[env:heltec_v3]` extending `esp32_base`,
      `board = heltec_wifi_lora_32_V3`, same lib_deps/I2S flags as `heltec_v4`,
      `-DDEVICE_HELTEC_V3`.
- [x] **P1.2** `src/pins.h`: added `#elif defined(DEVICE_HELTEC_V3)` branch with
      identical radio/OLED/I2S/key pins to V4, no `HAS_FEM`, no `PIN_FEM_*`,
      `PIN_VBAT_ADC=1 / PIN_VBAT_CTRL=37`, and `BATT_GATE_ACTIVE_HIGH=0`.
- [x] **P1.3** `src/battery.cpp`: parameterised gate polarity via
      `BATT_GATE_ACTIVE_HIGH` (1=V4 active-HIGH, 0=V3 active-LOW); V4 path is
      byte-identical when macro=1.
- [x] **P1.4** `display.cpp`, `sidetone.cpp`: guards changed from
      `#ifndef DEVICE_CARDPUTER_ADV` to `#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)`.
      `platform_esp32.cpp`, `kv_esp32.cpp`: added `|| defined(DEVICE_HELTEC_V3)`.
      `radio.cpp`: added V3 to the `TCXO_V` guard (same 1.8 V TCXO as V4).
      `HAS_FEM` blocks in radio.cpp compile out cleanly for V3 (no define).
- [x] **P1.5** All three builds pass:
      `heltec_v3` — 625,521 B flash / 34,748 B RAM (SUCCESS).
      `heltec_v4` — 625,781 B flash / 34,684 B RAM (SUCCESS, matches baseline).
      `cardputer_adv` — 761,173 B flash / 34,920 B RAM (SUCCESS, matches baseline).
- [x] **P1.6** V3 **hardware-validated** (station 38, 2026-06-11). The one fix a
      real unit needed: the V3's USB-C is a CP2102/UART0 bridge (not native USB
      like the V4), so `[env:heltec_v3]` overrides `-DARDUINO_USB_CDC_ON_BOOT=0`
      to route the console to UART0. Confirmed on hardware: serial console + BLE,
      `pwr 0..3`, `txcw` carrier at **−0.53 ppm** (stable TCXO, no FEM), watchdog
      (`bootlog reason=6(TASK_WDT)`), battery active-LOW gate (−1/no-cell, none
      attached). Station tools updated for the CP2102 port (`scripts/station_serial.py`).
      See `docs/components/heltec-v3.md`.

---

## Multi-target port — Phase 0 (seams only, zero behavior change)

- [x] **P0.1** `platformio.ini`: moved ESP32-only flags (`platform`, `framework`,
      `-DARDUINO_USB_*`) out of `[env]` into a new `[esp32_base]` section;
      both envs now `extends = esp32_base` and reference
      `${esp32_base.build_flags}`. Effective flags for both envs are unchanged.
- [x] **P0.2** `src/platform.h` + `src/platform_esp32.cpp`: introduced the
      `platform::` seam (restart / system_off / reset_reason / unique_id_byte).
      `main.cpp` no longer includes `<esp_sleep.h>` / `<esp_system.h>`.
      `config.cpp` no longer calls `ESP.getEfuseMac()` directly.
- [x] **P0.3** `src/kv.h` + `src/kv_esp32.cpp`: introduced the `kv::Store` seam.
      `config.cpp` and the `main.cpp` bootlog ring use `kv::Store` instead of
      `Preferences`; NVS namespace/keys are unchanged.
- [x] **P0.4** Both envs (`heltec_v4`, `cardputer_adv`) build clean.
      **USER GATE:** hardware-validate on Heltec V4.2, V4.3, Cardputer ADV
      before starting Phase 1. Flash with `scripts/devices.sh` + `--no-stub`
      fallback; check boot banner + reason line, `show`, `bootlog`, fox/hunter
      operation, sidetone, mute, battery.

---

## Stage 1 — Audio out (sidetone)

- [~] Wire the amp: **PAM8403** class-D board (Amazon B0DPMNYR2B) driving a
      **4 Ω** speaker (Amazon B0F3DBRXS5). PAM8403 takes an analog audio input
      and runs off 2.5–5 V — power it from the Heltec 3V3/5V rail, common GND.
      Pin: **GPIO4** → ~1 kΩ series + 100 nF → amp L_IN. (GPIO4, not GPIO7 —
      GPIO7 is FEM power on the V4.) See `docs/components/pam8403.md`.

## Stage 3 — Radio link

- [ ] **Polish/range + compliance:** TX gain mismatched between revisions
      (V4.2 ~+34 dB hotter than V4.3 — different FEM TX path/PA mode under the
      superset config). An SDR several floors away hears us at −80..−90 dBm with
      **no RX gain**, i.e. the V4.2 PA is clearly engaging and we are NOT in the
      +2 dBm low-power regime we configured. Range is plentiful for the camp
      either way. Before any deliberate outdoor deploy: equalise both boards to a
      matched power and decide the legal basis — §15.249 low-power unlicensed vs
      operating under an amateur license in the 902–928 MHz (33 cm) band (see
      `docs/protocol.md`). Not blocking the hunt.
- [~] **Heltec V4 FEM PA — engaged in TX modes; per-board EIRP calc still open.**
      **Done:** `setup()` now engages the FEM PA (`radio::set_pa(true)`) in the
      transmit run modes (Fox / Live Key) and leaves it bypassed in Hunter (RX);
      the `pa` console command is a runtime override on top of that default. This
      makes the V4 a deliberate higher-power fox rather than relying on the V4.2
      GC1109 PA engaging by accident. The LO/MED/HI/MAX labels remain **verbally
      approximate**, so the accepted consequence is that the same MAX radiates
      **~+28 dBm on the V4** (chip +22 + ~6 dB FEM PA) vs **+22 dBm on the Wio /
      no-FEM boards**. Documented in `docs/protocol.md`.
      **Still open (long-term):** correct per-board EIRP calculation. (1) The PA
      gain differs per rev (GC1109 vs KCT8103L) and is currently assumed ~+6 dB,
      not measured; read/track the real gain per revision. (2) Consider switching
      the PA per power level (e.g. bypass at LO/MED, engage at HI/MAX) instead of
      always-on in TX. (3) Reflect the chip-dBm → antenna-EIRP mapping in the
      `pwr` command / `show` output so the operator sees true EIRP, not just the
      chip figure. (4) Settle the legal basis (§15.249 vs Part 97) before any
      boosted outdoor deploy. See `docs/components/heltec-v4.md`.

## Stage 6 — Fox-hunt integration

- [ ] **New "standby" mode — radio off, BLE alive.** A mode that neither
      transmits nor receives on the SX1262 and powers down the radio + RF
      amplifiers (SX1262 to sleep; on the Heltec drop VFEM/FEM, on the Cardputer
      no FEM), **but keeps NimBLE up and responsive** so an operator can stage,
      transport, or pre-position a node and then arm it over the air. Unlike
      `hibernate()` (deep sleep, only RST wakes, BLE dead), standby keeps the CPU
      + BLE running and the display can show a "STANDBY" status. Add `MODE_STANDBY`
      to the `Mode` enum and the boot menu, and make it remotely selectable: an
      operator sends `mode <standby>` + `reboot` to park a node, later `mode 0/1/2`
      + `reboot` to bring it on-air. Implementation: skip `radio::init()` (or init
      then `radio::sleep()`), guard the per-mode `loop_*` so the main loop only
      services `ble_provision::process()` + display, and add a power-down that
      also drops the FEM rail on the Heltec. Note `mode <n>` currently clamps to
      0..2 in `handle_setup_command`; extend it. (`src/main.cpp`, `src/radio.cpp`.)
- [ ] **BLE notify throughput — long replies drop chunks.** Replies longer than
      a few hundred bytes (e.g. `bootlog`, a full `help`) arrive truncated/garbled
      over BLE NUS: the dump is oldest-first and the *tail* (newest bootlog
      entries) is consistently lost. Cause: `BleOut::emit` in
      `src/ble_provision.cpp` fires back-to-back 20-byte `notify()` calls with no
      pacing or flow control, so notifications are dropped when they outrun the
      connection interval. Observed on stn38 (NimBLE 2.x): 4 retries of `bootlog`
      only ever returned 6–10 of ~16 entries, max `#112` while the device was
      actually at `#116`. Short replies (`show`, `batt`) are fine. Fix options:
      negotiate a larger ATT MTU and chunk to `MTU-3` instead of 20; and/or add a
      small inter-notify yield / wait-for-txbuf so the stack drains. The nRF52
      Bluefruit `BleOut` (`ble_provision_nrf52.cpp`) likely needs the same. Purely
      a console-readability bug — does not affect provisioning or the MCU.
- [ ] Field test at range; tune power and message cadence (hardware).
- [ ] "RECV" station ID and signal strength bar should clear after timeout from
      last received packet.
- [ ] When receiving from one Fox (not timed out), packets from other stations
      must be ignored.

## Stage 7 — Range & polish

- [ ] Field-test low-power range across the camp area.
- [ ] **Later phase, only if range falls short:** FHSS — 50-channel hop table +
      RX scan-and-lock for §15.247 full-power operation. Design sketch in
      `docs/protocol.md`.
- [ ] Callsign ID: fox sends a periodic station-ID packet (`proto::Ident`, 8-min
      cadence, `tx_ident` in `src/main.cpp`) for Part 97 operation; the callsign
      also rides in the fox message text for an audible CW ID. Still missing an
      end-of-communication ID (no clean fox-exit path today). Only relevant under
      an amateur license — see `docs/protocol.md`.
- [ ] Enclosure, antenna build (hardware).

---

## Cardputer ADV port (experimental fox)

See `docs/components/cardputer-adv.md` for the hardware facts and lessons.

### Stage C1 — Fox bring-up on hardware

- [ ] **Sidetone (needs an ear):** confirm a clean keyed tone from the onboard
      speaker / 3.5 mm jack via M5.Speaker at the fox WPM; volume sane.
- [~] **Volume / mute control for the Cardputer.** Mute is done (see `DONE.md`).
      Still TODO: a graduated `vol <0..n>` level (M5.Speaker master volume) for
      finer control. (`sidetone_cardputer.cpp`, `handle_setup_command`/`loop` in
      `src/main.cpp`, `config.{h,cpp}`.)
- [ ] **On-air TX (needs a hunter):** confirm a Heltec hunter copies the
      Cardputer fox on 905.0 MHz. Check `setOutputPower` LO/MED/HI/MAX (no FEM →
      MAX +22 dBm is the real ceiling). G0 tap should cycle power.
- [ ] **Poor RX sensitivity on the Cardputer.** As a hunter the cap reads weak —
      the RSSI gauge sits ~halfway with devices ~1 ft apart, where a Heltec pins
      near full. The cap is a bare SX1262 with **no LNA/FEM**, so some gap is
      expected, but halfway at a foot is worse than that. Diagnose: confirm the
      RP-SMA antenna is seated, compare raw RSSI cap vs Heltec at a fixed
      distance, recheck the cap's SX1262 RX config (rxBw, no accidental gain
      reduction / `setRxBoostedGainMode`), and verify the shared SPI/SD bus isn't
      degrading the link. Decide whether the cap is RX-capable enough to be a
      hunter or is fox-only. (`src/radio.cpp`, `loop_hunter` in `src/main.cpp`.)
- [ ] Fox `setOutputPower` LO/MED/HI/MAX + G0 power-cycle; Ident cadence; LCD "*".

### Stage C2 — Keyboard entry

- [ ] **Verify on hardware:** keymap/shift correctness (esp. symbols), debounce
      feel, that the opt-in window times out cleanly, and that edited values
      persist + drive the fox loop.
- [ ] **Intermittent crash while typing a message on the keyboard**, followed by
      several reboots (seen once on HW 2026-05-31, did not reproduce). No serial
      capture of the panic yet — the USB port re-enumerates on the reboot so a
      plain `cat` of the port dies; capture with a reconnecting loop and watch for
      the ESP32 backtrace. Suspects: TCA8418 FIFO handling / keynum decode in
      `keyboard_cardputer.cpp`, or the editor buffer in `config_ui_cardputer.cpp`
      (bounds on append?). The `# boot #N reason` NVS log records the reset reason.
- [?] Decide whether keyboard config also covers wpm/farns/id, or those stay
      serial-only (currently serial-only).

### Stage C3 — Polish (later)

- [ ] Confirm legal power basis at +22 dBm (no FEM) — see `docs/protocol.md`.
- [ ] Optional: use the cap's ATGM336H GNSS to put the fox's grid/coordinates in
      the message automatically.
- [ ] Battery/run-time check (1750 mAh onboard) for a fox left running.
- [ ] Live-key mode: needs a physical key on a free GPIO (Grove G1/G2 — confirm);
      out of scope for the experimental fox.
