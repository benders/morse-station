# Per-device carrier frequency drift

Each station's SX1262 runs off its own TCXO, so every transmitter's carrier
sits a few hundred Hz off the nominal channel. This document records the
measured per-device offset and how to reproduce it.

Nominal channel: **905.000000 MHz** (`FREQ_MHZ` in `src/radio.cpp`).

## Measurement (2026-06-09)

Carrier keyed with the firmware `txcw` command and read with an RTL-SDR
(Nooelec SMArt, 0.5 ppm TCXO) served over `rtl_tcp`, using
`scripts/sdr_drift.py`. Two reads per station; carrier keyed over BLE.

| Station | Board | SoC | Carrier (MHz) | Drift | ppm | Jitter |
|---------|-------|-----|---------------|-------|-----|--------|
| 42  | heltec-v4.2     | ESP32-S3  | 904.999252 | −748 Hz | −0.826 | 2–7 Hz |
| 26  | rak4631         | nRF52840  | 904.999342 | −658 Hz | −0.727 | ~1 Hz  |
| 115 | wio-tracker-l1  | nRF52840  | 904.999352 | −648 Hz | −0.716 | ~1 Hz  |
| 43  | heltec-v4.3     | ESP32-S3  | 904.999544 | −456 Hz | −0.504 | 7–11 Hz |

- Total spread across all four devices: **~292 Hz (0.32 ppm)** — all healthy,
  no outliers.
- The two nRF52 boards (26, 115) cluster within 10 Hz of each other; the two
  Heltecs bracket them (42 lowest, 43 highest).
- Carriers are steady CW (jitter 1–11 Hz, SNR ~90 dB), so the estimate is
  precise to well under the dongle's own reference error.

### Absolute vs. relative

The **relative** differences between stations are the real per-device drift —
the dongle's reference error is common-mode and cancels (use `--ref <id>` for a
relative table). The shared ~−650 Hz offset on every station is mostly the
dongle: the Nooelec's ~0.5 ppm is ≈ −450 Hz at 905 MHz, right in this range. To
trust the **absolute** numbers, calibrate the dongle once against a known
reference and pass the correction with `--ppm`.

## Fixed: Cardputer ADV (Cap LoRa-1262) TCXO is 3.0 V, not 1.8 V (2026-06-10)

The Cardputer ADV (station 73) was the one bad actor. Its **Cap LoRa-1262** is a
removable module, and at the firmware's original `TCXO_V = 1.8f` its carrier did
**not** hold a stable reference. Measured with the same `txcw` + `sdr_drift.py`
rig, back-to-back 4 s reads landed at +6.6, −31.9, −31.9, −31.9 kHz, etc.:

| Station | Board | TCXO_V | Carrier behaviour | SNR | Jitter |
|---------|-------|--------|-------------------|-----|--------|
| 73 | cardputer-adv | **1.8 V** | wanders +7 to −47 ppm (~50 kHz range) | ~35 dB | up to 25 kHz |
| 73 | cardputer-adv | **3.0 V** | stable **−0.89 ppm** (−807 Hz), ~7 Hz spread | 89 dB | 6–120 (avg'd) |

The dongle was never at fault: the 42/43 anchors held their known values in the
very same runs.

**Root cause + fix.** The module's SX1262 TCXO runs at **3.0 V**, not 1.8 V —
confirmed by M5Stack's authoritative Arduino example, which calls
`radio.begin(..., 3.0, true)` (docs.m5stack.com/en/cap/Cap_LoRa-1262 → Arduino
Quick Start; the 9th/10th args are `tcxoVoltage` and `useRegulatorLDO`). At the
wrong 1.8 V the TCXO was underpowered and wandered. A control test at `TCXO_V=0`
(plain-crystal mode, no DIO3 supply) killed init entirely, proving a real TCXO is
present. `src/radio.cpp` now sets `TCXO_V = 3.0f` and `USE_LDO = true` for
`DEVICE_CARDPUTER_ADV` only; after reflashing, station 73's carrier sits at
−0.89 ppm — indistinguishable from the on-board-TCXO Heltec/RAK/Wio boards and
well inside the 23.4 kHz RX filter.

**Why it mattered:** against the narrowed 23.4 kHz RX filter (below), the
1.8 V-wandering carrier spent much of its time *outside* a receiver's passband, so
a Cardputer fox/instructor was intermittently off-channel. It still worked — the
instructor remote-control test (73 → fox 42, hunters 43/115) delivered the
command, got its
ACK, and both hunters copied the new message — but took ~18 s because the
instructor had to fire many bursts before one landed in-filter. With the 3.0 V
fix that delivery is now prompt; widening the receiving fox (e.g. `rxbw 117`) is
no longer needed for a Cardputer instructor.

## Added: Heltec V3 (station 38, 2026-06-11)

A new **Heltec V3** (ESP32-S3, on-board SX1262, no FEM) was brought to parity and
measured against an `rtl_tcp` dongle on `whitebox.lan`, `txcw` keyed over its USB
serial console:

| Station | Board | SoC | Carrier (MHz) | Drift | ppm | SNR |
|---------|-------|-----|---------------|-------|-----|-----|
| 38 | heltec-v3 | ESP32-S3 | 904.999519 | −481 Hz | −0.532 | 92 dB |

A stable on-board TCXO (1.8 V, like the V4 — cross-checked against MeshCore
`variants/heltec_v3`), squarely in the healthy <1 ppm population, nothing like
the Cardputer's pre-fix wander. Numbers are *not* folded into the 2026-06-09
table above: different dongle host and keyed over USB rather than BLE, so the
common-mode reference offset isn't directly comparable. The relative figure is
consistent with the others to well within the dongle's own error.

The V3's USB port is a **CP2102 bridge** (`/dev/cu.usbserial-*`), not native USB,
so it resets on open and the console is silent unless it does; `sdr_drift.py`
(and the other station tools) handle this via `scripts/station_serial.py`, so
`--station 38` / `--stations …,38` work the same as for a native-USB board.

## Consequence: RX bandwidth narrowed (78.2 → 23.4 kHz)

This drift measurement directly settled a long-standing RX-filter question. The
SX1262 FSK receive filter was set to **78.2 kHz** — far wider than the ~15 kHz
Carson signal width (4.8 kbps / 5 kHz dev) — on the theory that board-to-board
TCXO trims differed by **12–31 kHz (13–34 ppm)**, a figure read off early
GFSK-*burst* spectra where the apparent offset was really demod/measurement
error. The clean CW numbers above (**<1 ppm, <~1 kHz** spread) demolish that
rationale: frequency error consumes ~1% of the old filter margin, and you would
need ~35 ppm of pairwise drift to reach the 78.2 kHz edge.

A round-robin link test (`scripts/foxhunt_roundrobin.py --rxbw`, all four
stations, edge keymode, pwr LO, on the bench) confirmed it on-air:

| RX BW    | worst-pair loss | 26↔115 (RAK↔Wio) |
|----------|-----------------|------------------|
| 78.2 kHz | 2.9 %           | 0–1 %            |
| 39.0 kHz | 2.9 %           | 1 %              |
| 23.4 kHz | 2.9 %           | 1 %              |
| 19.5 kHz | 2.9 %           | 0 %              |

All four are statistically identical (1–3 packets out of ~100). In particular
the RAK↔Wio link (26↔115) — which an earlier `src/radio.cpp` comment claimed was
"100 % deaf" at 39 kHz — passes cleanly at every width. As the filter narrows the
hunter RSSI medians drop ~2–3 dB, consistent with the shrinking noise floor (the
predicted ~3 dB/octave sensitivity gain).

The firmware default is now **23.4 kHz** (~Carson 15 kHz + ~9.5 ppm cushion,
~5 dB more sensitivity than 78.2). It is runtime-settable and NVS-persisted via
the `rxbw <khz>` console command (snaps to the nearest SX1262 step; shown in
`show`), so any unit can be re-widened in seconds if a real offset ever appears.

**Caveat:** the round-robin above is a *strong-signal* bench test (RSSI −14 to
−67 dBm). It proves narrowing does no harm and refutes the drift rationale, but
the weak-signal **range** gain from the narrower filter still needs a field
distance test.

## Reproducing

On the host with the dongle (replace host as needed):

```sh
rtl_tcp -a 0.0.0.0                              # NOT sdr++ server — it speaks a
                                                # different protocol on :5259
```

Then, from the repo:

```sh
# passive: key `txcw 30` on a station yourself, then
~/.platformio/penv/bin/python scripts/sdr_drift.py --host <sdr-host> --once

# driven over USB serial, one or many stations into a table:
... scripts/sdr_drift.py --host <sdr-host> --stations 42,43,26,115 --repeat 2

# relative table against a reference station (dongle-independent):
... scripts/sdr_drift.py --host <sdr-host> --stations 42,43,26,115 --ref 43
```

To key a carrier by hand from any console (serial / BLE / `relay`):

```
txcw 12        # unmodulated carrier at 905.000 MHz for 12 s (cap 120)
txcw off       # stop early
```

`txcw` uses the current `pwr`/`pa` setting; keep power low and bursts short
(a pure carrier is a continuous emission under FCC §15.249).

To re-run the RX-bandwidth A/B over USB (sets the filter on every station, sweeps
fox turns, prints the loss/RSSI matrix per width):

```sh
~/.platformio/penv/bin/python scripts/foxhunt_roundrobin.py \
    --stations 42,43,115,26 --duration 30 --pwr 0 --rxbw 23.4
```

Or set one station's filter live from any console: `rxbw 23.4` (snaps to the
nearest SX1262 step, persists), `rxbw` / `rxbw show` to report.

See `scripts/sdr_drift.py` for the SDR side and `radio::tx_cw` in
`src/radio.cpp` for the firmware side.
