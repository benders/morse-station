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
- [x] **V4 external FEM (PA+LNA) brought up** — `radio::fem_enable()` powers
      VFEM (GPIO7), enables the FEM, sets PA-bypass, and `setDio2AsRfSwitch`
      lets DIO2 switch TX/RX. Without it RX was dead (~-107 dBm). One image
      drives the superset of V4.2 (GC1109) and V4.3.1 (KCT8103L) control pins.
      See memory `heltec-v4-fem-rf-frontend`.
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
- [ ] Field test at range; tune power and message cadence (hardware).

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
      `scripts/provision.sh` (--call/--msg/--id/--show). Builds clean; **not yet
      exercised against hardware** — DTR/RTS auto-reset is best-effort and may
      need a manual RST tap.
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
