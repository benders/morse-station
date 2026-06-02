# Component notes — Heltec WiFi LoRa 32 V4

ESP32-S3 + on-board **SX1262** + **external RF front-end (FEM: PA + LNA)** +
0.96″ SSD1306-class OLED + on-board LiPo charger. The original and primary
target (`-DDEVICE_HELTEC_V4`). Two units on hand. The radio itself is covered in
`sx1262.md`; this doc is the board-specific wiring and the hard-won lessons.

## Pin map (`src/pins.h`)

| Function | GPIO | Notes |
|---|---|---|
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 | on-board, fixed |
| SX1262 NRST / BUSY / DIO1 | 12 / 13 / 14 | |
| FEM VFEM (LDO power) | 7 | both revs |
| FEM CSD (chip enable) | 2 | both revs |
| FEM CTX | 5 | **V4.3 only** — SW TX/RX + LNA select; free GPIO on V4.2 |
| FEM CPS (PA mode) | 46 | **V4.2 only** — LOW = bypass |
| OLED SDA / SCL / RST | 17 / 18 / 21 | I2C |
| VEXT peripheral rail | 36 | **active-LOW** — drive LOW to power the OLED |
| Battery ADC / gate | 1 / 37 | divider on G1, gate G37 **active-HIGH** |
| Sidetone out (→ PAM8403) | 4 | was GPIO7 (that's FEM power) |
| Telegraph key (→ GND) | 6 | INPUT_PULLUP; was GPIO5 (FEM ctrl) |
| Mode button (PRG/BOOT) | 0 | on-board |

## Lessons learned

### The FEM is the single biggest source of rework

The V4 is the "high-power" Heltec variant: an external PA+LNA sits between the
SX1262 and the antenna. Key facts that cost real debugging time:

- **The FEM must be powered/enabled even for RX.** Leaving it off desensitises
  receive by tens of dB (we saw a dead RX at ~−107 dBm). `fem_power_on()` powers
  VFEM, asserts CSD, and sets the PA to bypass at init.
- **Two board revisions wire it differently** — the firmware drives the
  *superset* of control pins (pins a given rev doesn't use are free GPIOs there):
  - **V4.2 (GC1109):** TX/RX switch (CTX) is on **DIO2** and flips automatically
    via `setDio2AsRfSwitch`. PA mode is `CPS` (GPIO46). Its LNA stays in the RX
    path whenever CSD=1.
  - **V4.3.1 (KCT8103L):** the TX/RX switch (CTX) is **GPIO5, driven in
    software** (not DIO2), and that level **also selects the RX LNA**:
    `CTX=LOW` → LNA in path, `CTX=HIGH` → TX / LNA bypass. So GPIO5 **must track
    TX vs RX** — `radio.cpp` calls `fem_set_tx()` / `fem_set_rx()` around every
    send/receive. Holding it HIGH (the original bug) left V4.3 RX permanently
    LNA-bypassed, reading tens of dB below an otherwise-identical V4.2.
  - Pin roles cross-checked against meshcore-dev/MeshCore PR #1867.
- **The PA mode is not switched per power level (open item).** `CPS` is set to
  bypass once at init and never touched again; `set_tx_power()` only changes the
  SX1262 chip output. So the four power levels (LO/MED/HI/MAX) move the *chip*
  output, not the FEM PA. Empirically the V4.2 PA appears to engage anyway (an
  SDR several floors away hears us at −80..−90 dBm with no RX gain), so the two
  revisions' real EIRP is **mismatched** (V4.2 ~34 dB hotter). To use the V4 as a
  deliberate higher-power fox, the PA mode pin needs to be driven per level and
  the two revs equalised to a known EIRP. Not blocking the hunt. See `TODO.md`.

### Battery ADC gate is active-HIGH (opposite of the V3)

The cell is read through a 390k/100k divider on **GPIO1**, gated by a MOSFET on
**GPIO37**. On the V4 (both 4.2 and 4.3) the gate is **active-HIGH** — drive it
HIGH to connect the divider and read, LOW to disconnect and stop the idle drain.
This is the **opposite** of the V3 "GPIO37 low" convention; carrying the V3
assumption over reads 0. `battery.cpp` pulses it HIGH only for the sample.

### Avoid GPIO5 and GPIO7 for your own wiring

Both are claimed by the radio front-end (GPIO7 = FEM power, GPIO5 = V4.3 FEM
control). The sidetone moved off GPIO7 to **GPIO4**, and the telegraph key moved
off GPIO5 to **GPIO6** (GPIO5's pull-up wouldn't hold — it read LOW even
floating).

### Flashing

`pio upload` can intermittently fail with "No serial data received" after the
USB port re-enumerates (e.g. after a `devices.py --usb` read), on **any** board —
it's port churn, not a cable. Recover with `esptool --no-stub`, which also
preserves NVS. (See the saved memory notes.)

## Audio & display

- **Sidetone** is a PWM tone on GPIO4 into a **PAM8403** class-D amp — see
  `pam8403.md`.
- **Display** is the SSD1306-class OLED via U8g2 (`display.cpp`).

## Debugging panics

A native-USB panic can lose its backtrace mid-reset. The firmware persists each
boot's reset reason to NVS and prints the previous boot's reason
(`# boot #N reason now=.. prev=..`). For live backtraces over UART0 (GPIO43) or
JTAG over USB-C, see **`../debug-heltec-v4.md`**.
