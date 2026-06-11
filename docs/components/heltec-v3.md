# Component notes — Heltec WiFi LoRa 32 V3

ESP32-S3 + on-board **SX1262** + 0.96″ SSD1306-class OLED + on-board LiPo
charger. The low-power sibling of the V4 (`-DDEVICE_HELTEC_V3`): **no external
RF front-end (no FEM)**, so the SX1262 chip output (+22 dBm ceiling) is the
antenna power. Radio internals are in `sx1262.md`; this doc is the board-specific
wiring and the V3-only lessons. Hardware-validated (station 38) 2026-06-11.

## Pin map (`src/pins.h`)

| Function | GPIO | Notes |
|---|---|---|
| SX1262 NSS / SCK / MOSI / MISO | 8 / 9 / 10 / 11 | on-board, fixed (same as V4) |
| SX1262 NRST / BUSY / DIO1 | 12 / 13 / 14 | |
| OLED SDA / SCL / RST | 17 / 18 / 21 | I2C |
| VEXT peripheral rail | 36 | **active-LOW** — drive LOW to power the OLED |
| Battery ADC / gate | 1 / 37 | divider on G1, gate G37 **active-LOW** (opposite V4) |
| I2S sidetone → MAX98357A (BCLK / LRC / DIN) | 48 / 4 / 47 | set in `platformio.ini`; no speaker wired on the in-hand unit |
| Telegraph key (→ GND) | 6 | INPUT_PULLUP; no key wired on the in-hand unit |
| Mode button (PRG/BOOT) | 0 | on-board |

No FEM: do **not** define `HAS_FEM` or any `PIN_FEM_*` — `radio.cpp`'s FEM blocks
compile out, and the `pa`/`lna` console commands report "no FEM on this board".

## Lessons learned

### The USB console lives on UART0/CP2102, not native USB

This is the one real firmware difference that bites. The V3's USB-C connector is
wired to an on-board **CP2102 USB-UART bridge on UART0** (GPIO43/44), **not** to
the ESP32-S3 native USB (GPIO19/20) like the V4. So a V3 enumerates as
`/dev/cu.usbserial-*` (a CP210x), not `usbmodem*`.

`esp32_base` sets `-DARDUINO_USB_CDC_ON_BOOT=1`, which maps `Serial` onto the
**native** USB CDC — fine on the V4 (its USB-C *is* native USB), but on the V3
that output has nowhere to go and the console is silent. `[env:heltec_v3]`
overrides it with `-DARDUINO_USB_CDC_ON_BOOT=0` so `Serial` maps to
UART0 → CP2102 → `/dev/cu.usbserial-*`. Flashing works either way (esptool drives
the ROM bootloader over UART0 regardless). The env also points
`upload_port`/`monitor_port` at `/dev/cu.usbserial*`.

### Talking to a V3 from host tools (CP2102 quirks)

The CP2102 behaves differently from a native-USB board, and every station script
(`scripts/station_serial.py`, used by `devices.py`, `sercmd.py`,
`watchdog_test.py`, `sdr_drift.py`) accounts for it:

- **Opening the port resets the board** (DTR/RTS are wired to the ESP32
  auto-reset circuit) — and, counter-intuitively, that reset is **required**:
  opened *without* it (DTR/RTS held low) the V3 console comes up **silent**. So
  the tools do a default open and wait ~3.5 s for the firmware to boot before
  the first command. `station_serial.open_console()` hides this.
- **The USB node does not drop when the MCU reboots** (the CP2102 is the USB
  device, not the ESP32). So `watchdog_test.py` holds one handle open across a
  watchdog reboot instead of close/reopen — closing would DTR-reset the board
  ourselves and mask the very event under test.
- **The CHIP ID is not the CP2102 serial.** That serial is usually the
  unprogrammed default `0001`, shared across V3 units and unrelated to the MCU.
  Use the real ESP32-S3 base MAC the firmware prints in `show`
  (`chip=F0:9E:9E:76:7B:D8`); `devices.py` substitutes it for bridge ports. If
  raw port-name collisions become a problem with several V3s plugged in at once,
  reprogram each CP2102's serial with `cp210x-cfg`.

### Battery ADC gate is active-LOW (opposite of the V4)

The cell is read through a 390k/100k divider on **GPIO1**, gated by a MOSFET on
**GPIO37**. On the V3 the gate is **active-LOW** — drive it LOW to connect the
divider and read, HIGH to disconnect. This is the **opposite** of the V4
(active-HIGH); the shared `battery.cpp` path selects the level via the
`BATT_GATE_ACTIVE_HIGH` macro (0 for V3). With no cell attached the read clamps
to `-1`/no-cell, as expected.

### Radio: same 1.8 V TCXO as the V4

The on-board SX1262 uses a 1.8 V TCXO (`TCXO_V = 1.8f`, `USE_LDO = false`),
cross-checked against MeshCore `variants/heltec_v3`. The measured carrier is a
stable TCXO: **904.9995 MHz, ≈−0.53 ppm**, in the same healthy population as the
V4/RAK/Wio boards (see `../frequency-drift.md`).

## Audio & display

Same as the V4 — I2S → MAX98357A sidetone (`max98357a.md`) and an SSD1306-class
OLED via U8g2 (`display.cpp`). Neither speaker nor key is wired on the current
unit, so those pins idle harmlessly.
