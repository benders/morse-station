# 915 MHz GFSK Morse Station — Design Notes

Successor to the ESP-NOW design (see `esp-now.md`). Same youth-group use case;
the move to 915 MHz GFSK is driven by range and link reliability across open
fields and through light tree cover, where 2.4 GHz falls off fast.

The radio protocol carries **live key state** (not just canned text) so students
can key to each other after the hunt. Hardware is the **Heltec WiFi LoRa 32 V4**
(ESP32-S3 + SX1262), two units for testing. Starting at low power on a single
channel — **FHSS postponed** (see the regulatory section). See `TODO.md` for the
current plan.

## Goals

- Wireless Morse code practice for youth-group use, **2–4 km LOS** range target.
- No license required → operate in the 902–928 MHz ISM band under FCC Part 15.
  (A licensed operator can run the "fox" under Part 97 instead — see *Regulatory*.)
- ~$40–50/unit, USB-rechargeable, robust enough for kids.
- One instructor "fox" can broadcast to many receivers simultaneously.

## Why 915 MHz GFSK

- Sub-GHz propagates dramatically better than 2.4 GHz around foliage, terrain,
  and bodies. Free-space path loss alone is ~8 dB lower at 915 MHz than 2.4 GHz;
  in real environments the gap is wider.
- **GFSK at low data rate** maximizes RX sensitivity. The SX1262 reaches roughly
  **−120 dBm at 4.8 kbps GFSK** vs. ESP-NOW's ~−90 dBm — a ~30 dB link-budget
  improvement, before counting the lower path loss.
- Constant envelope → PA stays in compression, full output, no AM distortion.
- Narrow occupied bandwidth (~25 kHz at 4.8 kbps) → tight RX filter → low noise
  floor and good rejection of in-band Wi-Fi/Bluetooth garbage.
- No infrastructure, no AP/router/DHCP.

## Hardware choice

**Heltec WiFi LoRa 32 V4** (ESP32-S3 + SX1262, ~$22–30).

- ESP32-S3 MCU + SX1262 radio + 0.96″ SSD1306-class OLED on one board, with an
  onboard LiPo charger (SH1.25-2 connector). No separate charger wiring needed.
- SX1262 is a pure FSK/GFSK part — simple software stack via RadioLib `SX1262`.
  (Note: the SX126x family dropped OOK; this design only needs GFSK.)
- The V4 is the **high-power variant**: an external RF front-end (PA + LNA) sits
  between the SX1262 and the antenna, pushing up to **+27 dBm (~500 mW)**. The
  FEM **must be powered/enabled or receive is badly desensitised** — see
  *Power architecture* and `src/pins.h`. Two board revisions (V4.2 GC1109,
  V4.3.1 KCT8103L) wire the FEM control pins differently; the firmware drives
  the superset.
- SMA or u.FL pigtail antenna connector populated from the factory.

See `boards.md` for the full board survey that led here.

## Regulatory constraint — frequency hopping (FHSS postponed)

This was historically the single biggest design constraint.

- FCC §15.249 (narrowband, non-hopping) caps 902–928 MHz emissions at ~94 mV/m
  at 3 m → roughly **−1 dBm EIRP**. Short range, but legal with zero extra work,
  and **this is the mode the current firmware runs** (single fixed channel, low
  power).
- FCC §15.247 allows up to **+30 dBm** in 902–928 MHz, but only for **frequency-
  hopping (FHSS)** or DSSS systems. FHSS requirements (simplified): ≥50 hopping
  channels if hop bandwidth <250 kHz, average dwell ≤0.4 s per channel per 20 s
  window, channels approximately equally used.
- **Implication for the full-range design: we must hop.** Pick e.g. 50 channels
  spaced 200 kHz apart from 903.0 to 912.8 MHz (well clear of cellular at 928+).
  Change channel on every TX. The SX1262 retunes in well under 1 ms.
- Both transmitter and receivers run the same pseudo-random hop sequence seeded
  from a fixed key + a shared epoch. Practical implementation: a 256-entry table
  indexed by `(packet_counter mod 50)`, with `packet_counter` derived from
  millis() / hop_interval. Receivers re-sync by listening across all channels
  until they catch a beacon, then lock to the schedule.

For a kids' first build, the easy path is to just run at low power (~0 dBm)
under §15.249 and accept the shorter range — that gets you maybe 200–400 m,
which is still better than ESP-NOW indoors. **The full-range design assumes
FHSS.**

**Licensed alternative (fox only).** If the instructor "fox" is run by a
licensed amateur (Technician class or above), it can operate on the 33 cm
amateur band (902–928 MHz) under **Part 97** instead, with much higher power and
**no FHSS requirement**. Two Part 97 conditions then apply: (1) the station must
**identify with the operator's callsign at least every 10 minutes** (a CW ID
fits naturally), and (2) **no encryption / no obscuring** of transmissions — any
FHSS hop sequence must be publicly documented, not secret. The current firmware
uses only a plain sync word (no encryption), so it is already compatible.

## Protocol

Same call as ESP-NOW: **state broadcast**, self-healing.

- TX sends the current key state every **30 ms** (vs. 20 ms on ESP-NOW).
  At 4.8 kbps GFSK with a short preamble + sync + 1-byte payload + CRC, on-air
  time is ~12 ms per packet. 30 ms gives margin for retune + dwell rules.
- Packet payload: `[hop_index, station_id, key_state, seq]` — 4 bytes.
- A 2-byte **sync word** identifies the network (`{0x2D, 0xD4}`, see
  `src/radio.cpp`); node ID 255 is broadcast.
- No ACKs. No retries. Dropped packet → next one arrives 30 ms later.
- At 30 WPM, a "dit" is 40 ms — one missed packet won't elide an element.

Slower data rate (e.g. 1.2 kbps GFSK) buys ~6 dB more sensitivity but pushes
per-packet airtime past 50 ms, which breaks the 30 ms broadcast cadence. Stick
with 4.8 kbps unless you redesign as edge events.

## Addressing

- Each station has a 1-byte station ID set in NVS/flash at first boot.
- Instructor "fox" sends to broadcast ID 255; all receivers act on it.
- Partner practice: two stations exchange IDs at pairing time and filter on
  source ID.
- The SX1262 supports hardware node/broadcast address filtering (RadioLib
  `setNodeAddress` / `setBroadcastAddress`) to drop foreign packets before the
  MCU sees them, reducing ISR load on receive-only nodes.

## Sidetone

Unchanged in principle from the ESP-NOW design: local sidetone is generated
**directly from the key input**, not routed through the radio. A 30 ms
round-trip would be an audible slap-back. Received key state from the radio
drives the same tone generator on the receiving end.

The ESP32-S3 has no true DAC, so the tone is a **PWM square wave** on
`PIN_SIDETONE` (GPIO4) into a **PAM8403** class-D amp and a 4 Ω speaker.
(GPIO7 — the obvious audio pin — is the FEM power-enable on the V4 and must be
kept clear of the radio front-end; see `src/pins.h`.)

## Power architecture

The Heltec V4 integrates the supply that the ESP-NOW build wired by hand:

```
USB-C ──► onboard charger ──► LiPo (SH1.25-2)
                                  │
                                  └──► onboard 3.3V regulator
                                           ├──► ESP32-S3
                                           ├──► SX1262
                                           ├──► RF front-end (PA+LNA, gated by VFEM)
                                           └──► VEXT peripheral rail (gated)
```

- Charge-while-use works out of the box, unlike the bare TP4056 in the ESP-NOW
  design.
- The **VEXT** rail (gated by `PIN_VEXT_CTRL`, GPIO36, active-LOW) powers the
  OLED and any external peripherals — pull it low at boot.
- The **FEM** must be enabled via `PIN_FEM_VFEM` (GPIO7) before transmit *or*
  receive; leaving it off desensitises RX by tens of dB. The PA mode / control
  pins differ by board revision (V4.2 vs V4.3.1) — `src/pins.h` documents both.
- Speaker amp (PAM8403) runs off the 3.3V rail or VBAT.

## Decoupling

Less critical than ESP-NOW (no Wi-Fi TX bursts), but the SX1262 + FEM still draw
sharp current spikes on transmit. The Heltec V4 populates the radio and FEM
decoupling on-board; if you add an external PA or long supply leads, put **10 µF
∥ 0.1 µF** at the load.

## Wiring summary

Radio, OLED, and FEM are fixed on the Heltec V4. External wiring only (confirm
against the silkscreen — see `src/pins.h`):

| ESP32-S3 pin | Connection |
|---|---|
| GPIO6 (INPUT_PULLUP) | Telegraph key → GND |
| GPIO4 (PWM) | PAM8403 IN → speaker |
| GPIO0 (PRG/BOOT) | Mode-select button (onboard) |
| VEXT / 3V3 | OLED + peripheral power (gated by GPIO36) |
| BAT / 3V3 | PAM8403 V+ |
| GND | Common ground |

Add a 0.1 µF cap across the telegraph key contacts for debounce.

> Avoid GPIO7 (FEM power) and GPIO5 (V4.3 FEM control) for your own wiring —
> both are claimed by the radio front-end on the V4.

## Antenna

- 1/4-wave monopole: **82 mm of 22 AWG solid wire**, or use the supplied
  SMA/u.FL whip.
- Counterpoise: add three 82 mm radial wires to GND for a materially better
  pattern. Skipping the radials costs ~3–6 dB.
- Vertical orientation on both ends. A horizontal whip on one and vertical on
  the other costs ~20 dB of cross-polarization loss.

## Software outline (Arduino / ESP32, RadioLib)

The real implementation lives in `src/radio.cpp` / `src/radio.h` (a thin wrapper
over RadioLib's `SX1262`). Sketch of the fox TX path:

```cpp
#include <RadioLib.h>
#include "pins.h"

SX1262 chip = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY);

const uint8_t MY_ID = 1;            // per-station
const uint8_t BROADCAST_ID = 255;
uint8_t SYNC_WORD[] = { 0x2D, 0xD4 };

// FHSS table (full-range build only): 50 channels, 903.0 MHz + i*0.2 MHz
float hopFreq(uint8_t i) { return 903.0f + i * 0.2f; }
uint8_t hopIndex(uint32_t seq) { return seq % 50; }

void setup() {
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_FEM_VFEM, OUTPUT);
  digitalWrite(PIN_FEM_VFEM, HIGH);          // FEM on (required for TX and RX)

  chip.beginFSK(915.0, 4.8, 9.6);            // 4.8 kbps, 9.6 kHz deviation
  chip.setSyncWord(SYNC_WORD, sizeof(SYNC_WORD));
  chip.setOutputPower(0);                     // low power; FEM PA sits after this
  // No encryption — keeps the door open for Part 97 fox operation.
}

uint32_t seq = 0;

void loop() {
  static uint32_t last = 0;
  bool down = (digitalRead(PIN_KEY) == LOW);

  // local sidetone — direct from key, never via radio (PWM on GPIO4)
  // (tone generation handled by the audio module)

  // broadcast every 30 ms (single channel for now; hop when FHSS lands)
  if (millis() - last >= 30) {
    last = millis();
    uint8_t h = hopIndex(seq);                // 0 until FHSS is enabled
    uint8_t pkt[4] = { h, MY_ID, (uint8_t)down, (uint8_t)(seq & 0xFF) };
    chip.transmit(pkt, sizeof(pkt));
    seq++;
  }

  // RX path: scan-and-lock state machine omitted for brevity
}
```

Receivers need a brief scan-across-all-channels state at boot to acquire the
hop sequence (full-range build), then lock to the schedule using the `seq` field
in received packets. See `rx-sync.md` (TODO) for that state machine.

## Range expectations

Rough budget at 4.8 kbps GFSK, 1/4-wave antennas, both ends vertical, line of
sight:

| Environment | Expected range |
|---|---|
| Open field, no obstructions | 3–5 km |
| Light tree cover | 1–2 km |
| Suburban / through buildings | 200–500 m |
| Indoor through walls | 50–150 m |

These assume the higher-power FHSS implementation (or a Part 97 fox). At ~0 dBm
for §15.249 compliance — the current firmware default — divide all numbers by
~10.

## Open questions / future work

- RX scan-and-lock state machine (`rx-sync.md`).
- FHSS implementation + verify hop dwell timing meets §15.247(a)(1)
  "approximately equal use" within a 20 s window. 30 ms × 50 channels = 1.5 s
  full cycle, so each channel sees ~13 hits per 20 s window — well-distributed.
- Callsign CW ID in the fox TX loop (required if run under Part 97).
- **No encryption** is used or planned — it would bar Part 97 operation and
  isn't needed for this use case.
- Bigger antenna (½-wave dipole, ~16 cm overall, balun-fed) for the instructor
  station — ~3 dB on TX and RX.
- OLED decoded-text display, RSSI for fox-hunt — carried forward from ESP-NOW
  design.
