# Component notes — Seeed Wio Tracker L1 Pro

**nRF52840** + **Wio-SX1262** (no FEM) + 128×64 mono OLED + built-in **passive
piezo buzzer** + on-board buttons, in a sealed handheld. Built with
`-DDEVICE_WIO_TRACKER_L1`. It is the **sibling nRF52 board** to the RAK4631 and
reuses the same backend (`platform_nrf52.cpp`, `kv_nrf52.cpp`,
`ble_provision_nrf52.cpp`, the global-SPI SX1262 path in `radio.cpp`), so the
shared nRF52-family lessons live in **[`rak4631.md`](rak4631.md)** — read that
first. This doc covers only what's **different** on the Wio. The radio itself is
in `sx1262.md`; full pin provenance, the schematic PDF, and the board diagram are
in `reference/wio-tracker-l1-pro/`. Hardware-validated (station 115): radio,
OLED, buzzer sidetone, keyer, BLE, watchdog.

## Shared with the RAK4631 (see `rak4631.md`)

The whole nRF52 vs ESP32 split — Adafruit core + SoftDevice S140, Bluefruit BLE,
LittleFS, global SPI, nrfutil DFU — and these lessons apply unchanged:

- the two boot-hang fixes (`InternalFS.begin()`, TWIM I2C bus-recover + headless
  guard);
- USB enumerates **before** `setup()`, and the 500 ms boot-banner wait;
- the **reset-cause** reconstruction via the flash reboot-intent flag (the Wio
  does **not** have the RAK's intentional-reboot double-boot quirk);
- **Bluefruit BLE notify must be chunked yourself** (HW-validated on the Wio:
  MTU 247, full `help`/`bootlog`);
- the bare SX1262 **RSSI offset** vs an FEM Heltec (real RX is fine, the bar
  misleads);
- nRF52 boards **can't be DTR/RTS-reset** — provision over the runtime console.

## Pin map (`src/pins.h` ← `platformio.ini`)

> **Pin numbers here are Arduino *logical* pins** (the D-index into the variant's
> `g_ADigitalPinMap`), **not** raw nRF GPIOs — this variant's map is **not
> identity** (the RAK4631's is). `radio.cpp`/`pins.h` call `pinMode`/RadioLib with
> these logical numbers; the raw `Pxx.yy` is shown in parentheses for reference.

| Function | D-pin (raw) | Notes |
|---|---|---|
| SX1262 NSS / SCK / MISO / MOSI | 4 / 8 / 9 / 10 | global SPI |
| SX1262 NRST / BUSY / DIO1 | 2 / 3 / 1 | |
| RF switch `LoRa_SW` | D5 (SX126X_RXEN) | DIO2 also asserted as RF switch upstream — DIO2 kept default |
| TCXO | DIO3, **1.8 V** | |
| **(no LoRa power-enable)** | — | always-on `LoRa_3V3` LDO — **do not** set `SX126X_POWER_EN` (the opposite of the RAK) |
| OLED SDA / SCL | 14 (6) / 15 (5) | **SH1106** controller, no reset line — see below |
| Buzzer | `PIN_BUZZER` = 12 (P1.00) | passive piezo, active-high square wave |
| Telegraph key (`Menu_Key`) | `PIN_KEY` = 13 (8) | a real button — keyer HW-validated |
| Mode button (`Joystick_Press`) | `PIN_MODE_BTN` = 29 | no separate "Rot_Key"; the 5-way joystick press is the menu button |
| Battery VBAT_ADC / gate `BAT_CTL` | 16 (31) / 30 (4) | drive `BAT_CTL` **HIGH** to enable the divider; `ADC_MULTIPLIER` = 2.0 |

No FEM (bare SX1262, +22 dBm chip ceiling) — same as the RAK.

## Wio-specific lessons

### SoftDevice S140 **7.3.0** → vendored v7 ldscript (boot-critical)

The unit ships with the Adafruit UF2 bootloader carrying **SoftDevice S140
7.3.0** (confirmed via `INFO_UF2.TXT`), whose app base is **0x27000**. The stock
framework only ships the v6 ldscript (app @ **0x26000**), so we **vendor a v7
ldscript** and point at it with `board_build.ldscript =
variants/Seeed_Wio_Tracker_L1/nrf52840_s140_v7.ld`. Without this the app links to
the wrong base and **silently won't boot**. (This is the headline difference from
the RAK, whose board JSON is S140 6.1.1 / v6.) This was the fix that first got
the port booting on hardware (commit `8f8c0df`).

### The OLED is an **SH1106**, not an SSD1306

The 128×64 panel uses an **SH1106** controller, which has **132 columns of RAM
with only the middle 128 visible** (a 2-column offset). Driving it with the
SSD1306 init shifts the image and distorts the right edge. `display.cpp` selects
the **SH1106 U8g2 constructor** for `DEVICE_WIO_TRACKER_L1` (which applies the
column offset); the RAK1921/Heltec stay on the SSD1306 constructor (commit
`4ad5d17`).

### Sidetone is the built-in passive buzzer (no amp, no I2S)

The Wio's only audio transducer is an on-board passive piezo buzzer (net
`BUZZER`, resonant ~2700 Hz), so sidetone is a **direct PWM square wave** on
`PIN_BUZZER` (via an NPN), selected by **`-DSIDETONE_BUZZER`** in
`sidetone_nrf52.cpp`. This is its *only* option — there is no MAX98357A/I2S or
PAM8403 path here (those are the RAK's). Simplest sidetone backend of any board;
the square wave sits near the buzzer's resonance so it's loud for the duty.

### Constant buzzer "ticking" was the Bluefruit auto conn-LED

Out of the box the buzzer ticked continuously. Cause: Bluefruit's **automatic
connection LED** drives `LED_CONN` (= `PIN_LED2` = **D12 = P1.00**) — the **same
pin as the buzzer**. The advertising/connect blink therefore pulsed the piezo at
the blink rate → a steady tick. Fix: **`Bluefruit.autoConnLed(false)`** in
`ble_provision_nrf52.cpp` so the BLE stack stops toggling the buzzer pin (commit
`93b9bdf`).

### Recovery: it's parked in the UF2 bootloader, not crashed

A Wio that looks "hung at boot / blank display" is almost always sitting in its
**UF2 mass-storage bootloader** (a `/Volumes/TRACKER L1` drive with
`INFO_UF2.TXT` is mounted), not crashed — it never handed off to the app. Recover
by drag-dropping a UF2: `pio run -e wio_tracker_l1`, convert the hex with the
Adafruit family id **`0xADA52840`** (verify it prints **`start address:
0x27000`** — the S140 7.3.0 base), `cp` it to the drive; the bootloader flashes,
auto-resets into the app, and the drive unmounts (the success signal). NVS-style
identity (station id / `model=wio-tracker-l1`) survives the reflash. The normal
`upload_protocol = nrfutil` (1200 bps touch) path also works.

## Status & known issues

Hardware-validated on station 115: radio TX/RX, SH1106 OLED, buzzer sidetone +
mute, the **keyer/Morse key** (commit `1e9511c`), BLE console, and the watchdog
(`reason=2(WATCHDOG)`, clean, no double-boot). The sealed enclosure means
validation is **buttons + console + OTA only** — no easy bare-GPIO access.

- **BLE-AUTO power saving is disabled** — both nRF52 boards are pinned `BLE_ON`,
  forfeiting the ~70 mA idle saving the ESP32 boards get from the AUTO
  panel-follow policy. Root cause: `ble_provision_nrf52`'s `stop()` only halts
  advertising without releasing the SoftDevice, so a later `begin()` re-inits an
  already-initialized stack and hangs (→ 8 s watchdog reset). The fix is to make
  `stop()`/`begin()` safe to cycle (mirroring the NimBLE path) and drop the
  `BLE_CYCLE_UNSAFE` pin. See
  [issue #3](https://github.com/benders/morse-station/issues/3).
</content>
