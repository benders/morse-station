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
- [ ] Flash `src/main.cpp` (Stage-1 beep test) and confirm audio on hardware.

## Stage 2 — Telegraph key input

- [ ] Wire the key to a GPIO with `INPUT_PULLUP`, key shorts to GND.
- [ ] Debounce (RC cap across contacts + software). Confirm clean edges on
      a scope or via serial timestamps.
- [ ] Key down → `sidetone_on()`, key up → `sidetone_off()`, locally, with no
      perceptible latency. This is the "straight key practice" loop, radio
      not involved yet.

## Stage 3 — Radio link bring-up (FSK TX/RX on two units)

Needs a second Heltec V4 flashed as the counterpart.

- [ ] Replace the scanner `main.cpp` with a switchable TX/RX build (build flag
      or runtime button) so one unit transmits and one receives.
- [ ] Establish a GFSK link on a single fixed channel: TX sends a known
      payload on a timer, RX prints received bytes + RSSI to serial.
- [ ] Confirm RadioLib SX1262 FSK config matches design notes (4.8 kbps,
      ~5 kHz dev, sync word identifying the net). Tune `rxBw`.
- [ ] Bench range / packet-loss sanity check at low power.

## Stage 4 — Fox message loop (pre-programmed transmit)

- [ ] Store the fox's location-description message as text in firmware
      (e.g. `"FOX NEAR THE BIG OAK BY THE LAKE"`).
- [ ] Text → Morse timing: map chars to dit/dah element sequences with correct
      WPM timing (dit/dah/intra-char/inter-char/inter-word gaps).
- [ ] Drive the local sidetone from the Morse timing so the fox audibly keys
      itself — confirms the encoder before any radio involvement.
- [ ] Loop the message with a configurable inter-repeat pause.

## Stage 5 — Morse over FSK — keystate broadcast

**Decided: keystate broadcast (design-notes approach A).** Students will want
to live-key to each other after the hunt, and only this scheme carries live
keying with real timing. The canned fox loop just feeds the same keystate
stream from the encoder instead of a human key, so one protocol covers both
uses.

- [ ] TX sends a small packet every ~30 ms carrying current key-down/up state
      (+ station id, seq). RX reconstructs timing and drives its sidetone.
      No ACKs, no retries — a dropped packet is corrected by the next one.
- [ ] Fox loop feeds the keystate stream from the Stage-4 Morse encoder rather
      than from a physical key — same wire format, same RX code.
- [ ] RX side: key-down/up stream → sidetone, and feed a local Morse *decoder*
      to render text on the OLED.
- [ ] End-to-end: fox (or a live key) → transmit → hunter hears Morse + sees
      decoded text on OLED.

## Stage 6 — Fox-hunt integration

- [ ] Fox mode: loops Stage-4 message, transmits via Stage-5 scheme, low duty
      cycle to save battery, OLED shows "FOX TX" status.
- [ ] Hunter mode: receives, plays Morse sidetone, shows decoded text + RSSI
      bar on OLED (RSSI as a crude "warmer/colder" hint).
- [ ] Mode select at boot (button or build flag).
- [ ] Field test at range; tune power and message cadence.

## Stage 7 — Range & polish

Start at **low power, single fixed channel, no hopping** (§15.249, ~200–400 m)
— expected to be enough for a camp. FHSS is postponed to a later phase.

- [ ] Field-test low-power range across the camp area.
- [ ] **Later phase, only if range falls short:** FHSS — 50-channel hop table
      + RX scan-and-lock (`rx-sync.md`, a TODO in design notes) for §15.247
      full-power operation.
- [ ] Per-unit station IDs in NVS/EEPROM, enclosure, antenna build.
