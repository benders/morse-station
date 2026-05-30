# 915 MHz GFSK Morse Station — Design Notes

> **Update (2026-05): hardware changed.** The Feather M0 / RFM69 path below is
> dropped. All work is now on the **Heltec WiFi LoRa 32 V4** (ESP32-S3 +
> SX1262, RadioLib `SX1262`), with two units for testing. Audio is a
> **PAM8403** class-D amp into a 4 Ω speaker, driven by ESP32-S3 PWM (no true
> DAC). Starting at low power, single channel — **FHSS postponed**. The radio
> protocol carries **live key state** (not just canned text) so students can
> key to each other after the hunt. See `TODO.md` for the current plan; the
> RFM69 pin maps and the §15.247 FHSS detail below remain useful reference.

Successor to the ESP-NOW design (see `esp-now.md`). Same youth-group use case;
the move to 915 MHz GFSK is driven by range and link reliability across open
fields and through light tree cover, where 2.4 GHz falls off fast.

## Goals

- Wireless Morse code practice for youth-group use, **2–4 km LOS** range target.
- No HAM license required → operate in the 902–928 MHz ISM band under FCC Part 15.
- ~$40–50/unit, USB-rechargeable, robust enough for kids.
- One instructor "fox" can broadcast to many receivers simultaneously.

## Why 915 MHz GFSK

- Sub-GHz propagates dramatically better than 2.4 GHz around foliage, terrain,
  and bodies. Free-space path loss alone is ~8 dB lower at 915 MHz than 2.4 GHz;
  in real environments the gap is wider.
- **GFSK at low data rate** maximizes RX sensitivity. RFM69HCW hits roughly
  **−120 dBm at 4.8 kbps GFSK** vs. ESP-NOW's ~−90 dBm — a ~30 dB link-budget
  improvement, before counting the lower path loss.
- Constant envelope → PA stays in compression, full +20 dBm out, no AM distortion.
- Narrow occupied bandwidth (~25 kHz at 4.8 kbps) → tight RX filter → low noise
  floor and good rejection of in-band Wi-Fi/Bluetooth garbage.
- No license, no infrastructure, no AP/router/DHCP.

## Hardware choice

**Primary: Adafruit Feather M0 RFM69HCW 900 MHz** (P/N 3176, ~$35).

- SAMD21 + RFM69HCW on one board; JST-PH battery connector and onboard LiPo
  charger built in. No TP4056 / load-sharing wiring needed.
- RFM69HCW is a pure FSK/GFSK/OOK part — simplest possible software stack
  (RadioHead `RH_RF69` or RadioLib `RF69`).
- +20 dBm TX, on-chip PA and LNA.
- u.FL **footprint** is not populated — hand-solder a u.FL jack and use a small
  external whip / 1/4-wave wire (~8.2 cm) for the range target.

Alternative: **Feather M0 RFM95 LoRa 900 MHz** (P/N 3178, ~$35) — same form
factor, SX1276 instead of RFM69. Gives you GFSK *and* LoRa as a fallback for
edge-of-range conditions. Worth it if you want LoRa later; otherwise the RFM69
is simpler and cheaper to reason about.

## Regulatory constraint — frequency hopping required

This is the single biggest design constraint and the reason this doc exists.

- FCC §15.249 (narrowband, non-hopping) caps 902–928 MHz emissions at ~94 mV/m
  at 3 m → roughly **−1 dBm EIRP**. Useless for our range target.
- FCC §15.247 allows up to **+30 dBm** in 902–928 MHz, but only for **frequency-
  hopping (FHSS)** or DSSS systems. FHSS requirements (simplified): ≥50 hopping
  channels if hop bandwidth <250 kHz, average dwell ≤0.4 s per channel per 20 s
  window, channels approximately equally used.
- **Implication: we must hop.** Pick e.g. 50 channels spaced 200 kHz apart from
  903.0 to 912.8 MHz (well clear of cellular at 928+). Change channel on every
  TX. RFM69 retunes in <1 ms.
- Both transmitter and receivers run the same pseudo-random hop sequence seeded
  from a fixed key + a shared epoch. Practical implementation: a 256-entry table
  indexed by `(packet_counter mod 50)`, with `packet_counter` derived from
  millis() / hop_interval. Receivers re-sync by listening across all channels
  until they catch a beacon, then lock to the schedule.

For a kids' first build, an easier path is to just run at low power (~0 dBm)
under §15.249 and accept the shorter range — that gets you maybe 200–400 m,
which is still better than ESP-NOW indoors. **The full-range design assumes
FHSS.**

## Protocol

Same call as ESP-NOW: **state broadcast**, self-healing.

- TX sends the current key state every **30 ms** (vs. 20 ms on ESP-NOW).
  At 4.8 kbps GFSK with a short preamble + sync + 1-byte payload + CRC, on-air
  time is ~12 ms per packet. 30 ms gives margin for retune + dwell rules.
- Packet payload: `[hop_index, station_id, key_state, seq]` — 4 bytes.
- RFM69 sync word identifies the network; node ID 255 is broadcast.
- No ACKs. No retries. Dropped packet → next one arrives 30 ms later.
- At 30 WPM, a "dit" is 40 ms — one missed packet won't elide an element.

Slower data rate (e.g. 1.2 kbps GFSK) buys ~6 dB more sensitivity but pushes
per-packet airtime past 50 ms, which breaks the 30 ms broadcast cadence. Stick
with 4.8 kbps unless you redesign as edge events.

## Addressing

- Each station has a 1-byte station ID set in EEPROM at first boot.
- Instructor "fox" sends to broadcast ID 255; all receivers act on it.
- Partner practice: two stations exchange IDs at pairing time and filter on
  source ID.
- RFM69 hardware address filtering reduces ISR load — set `setAddress()` and
  `setPromiscuous(false)` on receive-only nodes.

## Sidetone

Unchanged from ESP-NOW design: local sidetone is generated **directly from the
key input**, not routed through the radio. 30 ms round-trip would be audible
slap-back. Received key state from the radio drives the same tone generator on
the receiving end.

## Power architecture

The Feather M0 collapses the entire ESP-NOW power section into the board:

```
USB-C ──► Feather USB ──► onboard MCP73831 charger ──► 3.7V LiPo (JST-PH)
                                                            │
                                                            └──► onboard 3.3V LDO
                                                                       │
                                                                       ├──► SAMD21
                                                                       ├──► RFM69HCW
                                                                       └──► EXT 3V3 rail
```

- Charge-while-use works out of the box — MCP73831 + Feather's path management
  is fine, unlike the bare TP4056 in the ESP-NOW design.
- Speaker amp (PAM8302) and LEDs come off the Feather's 3.3V or BAT pin.
- A protected 18650 in a JST-PH holder is the easiest cell; or a 1200 mAh
  pouch LiPo for compactness.

## Decoupling

Less critical than ESP-NOW (no Wi-Fi TX bursts), but still: **10 µF tantalum ∥
0.1 µF ceramic** between the RFM69's 3.3V pin and ground, right at the module.
On the bare RFM69 FeatherWing this is on the wing; on the integrated Feather
RFM69 it's already populated.

## Wiring summary

The radio is pre-wired on the Feather M0 RFM69. External wiring only:

| Feather pin | Connection |
|---|---|
| 3V | Sensors / OLED VCC |
| GND | Common ground |
| BAT | PAM8302 V+ (3.7–4.2 V direct) |
| D11 (INPUT_PULLUP) | Telegraph key → GND |
| A0 (true DAC) | PAM8302 IN+ → speaker |
| D13 | Signal LED (already on board) |
| SDA / SCL | OLED if used |
| RFM69 RST, CS, IRQ | pre-wired (D4, D8, D3 on M0 RFM69) |

Add a 0.1 µF cap across the telegraph key contacts for debounce.

## Antenna

- 1/4-wave monopole: **82 mm of 22 AWG solid wire** soldered to the ANT pad
  (or to the center of a hand-mounted u.FL jack with a short pigtail).
- Counterpoise: the Feather ground plane is small — add three 82 mm radial
  wires to GND for materially better pattern. Skipping the radials costs
  ~3–6 dB.
- Vertical orientation on both ends. A horizontal whip on one and vertical on
  the other costs ~20 dB of cross-polarization loss.

## Software outline (Arduino, RadioHead)

```cpp
#include <SPI.h>
#include <RH_RF69.h>

#define RFM69_CS   8
#define RFM69_INT  3
#define RFM69_RST  4
#define KEY_PIN    11
#define LED_PIN    13
#define TONE_PIN   A0
#define TONE_HZ    600

RH_RF69 rf69(RFM69_CS, RFM69_INT);

const uint8_t MY_ID = 1;            // per-station
const uint8_t BROADCAST_ID = 255;

// FHSS table: 50 channels, 903.0 MHz + i*0.2 MHz
float hopFreq(uint8_t i) { return 903.0f + i * 0.2f; }
uint8_t hopIndex(uint32_t seq) { return seq % 50; }

void setup() {
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RFM69_RST, OUTPUT);
  digitalWrite(RFM69_RST, LOW);

  rf69.init();
  rf69.setModemConfig(RH_RF69::GFSK_Rb4_8Fd9_6);  // 4.8 kbps, 9.6 kHz dev
  rf69.setTxPower(20, true);                       // +20 dBm, HCW mode
  rf69.setSyncWords((uint8_t[]){0x2D, 0xD4}, 2);
  rf69.setEncryptionKey((uint8_t*)"morse-camp-2026!");
}

uint32_t seq = 0;

void loop() {
  static uint32_t last = 0;
  bool down = (digitalRead(KEY_PIN) == LOW);

  // local sidetone — direct from key, never via radio
  if (down) tone(TONE_PIN, TONE_HZ); else noTone(TONE_PIN);

  // hop + broadcast every 30 ms
  if (millis() - last >= 30) {
    last = millis();
    uint8_t h = hopIndex(seq);
    rf69.setFrequency(hopFreq(h));
    uint8_t pkt[4] = { h, MY_ID, (uint8_t)down, (uint8_t)(seq & 0xFF) };
    rf69.send(pkt, sizeof(pkt));
    rf69.waitPacketSent();
    seq++;
  }

  // RX path: scan-and-lock state machine omitted for brevity
}
```

Receivers need a brief scan-across-all-channels state at boot to acquire the
hop sequence, then lock to the schedule using the `seq` field in received
packets. See `rx-sync.md` (TODO) for that state machine.

## Range expectations

Rough budget at 4.8 kbps GFSK, +20 dBm TX, 1/4-wave antennas, both ends
vertical, line of sight:

| Environment | Expected range |
|---|---|
| Open field, no obstructions | 3–5 km |
| Light tree cover | 1–2 km |
| Suburban / through buildings | 200–500 m |
| Indoor through walls | 50–150 m |

These assume the FHSS implementation is correct. Drop power to ~0 dBm for
§15.249 compliance and divide all numbers by ~10.

## Open questions / future work

- RX scan-and-lock state machine (`rx-sync.md`).
- Verify hop dwell timing meets §15.247(a)(1) "approximately equal use" within
  a 20 s window. 30 ms × 50 channels = 1.5 s full cycle, so each channel sees
  ~13 hits per 20 s window — well-distributed.
- Optional: AES encryption is on by default (RFM69 hardware AES-128) — pin a
  shared key per camp session.
- Bigger antenna (½-wave dipole, ~16 cm overall, balun-fed) for the instructor
  station — ~3 dB on TX and RX.
- OLED decoded-text display, RSSI for fox-hunt — carried forward from ESP-NOW
  design.
