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

## Outlier: Cardputer ADV (Cap LoRa-1262) has no usable TCXO (2026-06-10)

The Cardputer ADV (station 73) is the exception. Its **Cap LoRa-1262** is a
removable module, and unlike every on-board SX1262 above it does **not** present
a stable reference. Measured with the same `txcw` + `sdr_drift.py` rig:

| Station | Board | Carrier behaviour | SNR | Jitter |
|---------|-------|-------------------|-----|--------|
| 73 | cardputer-adv (Cap LoRa-1262) | wanders **+7 to −47 ppm** (+6.6 kHz to −43 kHz) between reads | ~35 dB | up to 25 kHz |

Back-to-back 4 s reads landed at +6.6, −31.9, −31.9, −31.9 kHz, etc. — a ~50 kHz
range — versus the <1 ppm, <100 Hz-jitter, 79–90 dB-SNR carriers of every TCXO
board. The dongle is not at fault: the 42/43 anchors held their known values in
the very same runs.

The firmware assumes a 1.8 V TCXO for this board (`TCXO_V = 1.8f` in
`src/radio.cpp`, flagged `// VERIFY ON HW: Cap LoRa-1262`). The module almost
certainly has a bare/uncompensated crystal instead, so commanding DIO3-TCXO mode
leaves an unstable, temperature-wandering XOSC. **Untested fix:** set `TCXO_V=0`
for `DEVICE_CARDPUTER_ADV` (use the crystal directly) and re-measure.

**Consequence:** against the narrowed 23.4 kHz RX filter (below), the Cardputer's
carrier spends much of its time *outside* a receiver's passband, so a Cardputer
fox/instructor is intermittently off-channel. It still works — the instructor
remote-control test (73 → fox 42, hunters 43/115) delivered the command, got its
ACK, and both hunters copied the new message — but took ~18 s because the
instructor had to fire many bursts before one landed in-filter. Widening the
receiving fox (e.g. `rxbw 117`) is the interim workaround until the TCXO config
is fixed.

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
