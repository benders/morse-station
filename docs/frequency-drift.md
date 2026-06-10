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

See `scripts/sdr_drift.py` for the SDR side and `radio::tx_cw` in
`src/radio.cpp` for the firmware side.
