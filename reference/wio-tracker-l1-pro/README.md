# Seeed Wio Tracker L1 Pro — reference

Product page: https://www.seeedstudio.com/Wio-Tracker-L1-Pro-p-6454.html

Schematic: `Wio_Tracker_L1_Pro_SCH_PDF.pdf` ("Meshtastic Repeater_Handle" rev
v1.3) and block diagram `L1_Diagram.png` (both vendored alongside this file).

## Upstream provenance (vendored in Phase W1)

**No L1-Pro-specific board/variant exists upstream.** The Meshtastic firmware
repo ships a board + variant for the plain **"Wio Tracker L1"** (OLED + GNSS,
no e-ink) and a separate `seeed_wio_tracker_L1_eink` variant (e-ink, no OLED/
`HAS_WIRE` differs). Since the L1 Pro in this project has the SSD1306 OLED
(per §0 of `wio-tracker-port.md` and the schematic's "OLED Interface" sheet),
the **plain L1 variant is the correct upstream base** — it is electrically the
sibling we need (SX1262 LoRa pins, OLED I2C pins, buzzer, battery-gate pins all
match the schematic's net names 1:1; see the pin-map table below).

Source: `github.com/meshtastic/firmware`, commit
`8c4900a52fca19f506f1a19834bb5371c1668d93` (fetched 2026-06-07):

- Board JSON: [`boards/seeed_wio_tracker_L1.json`](https://github.com/meshtastic/firmware/blob/8c4900a52fca19f506f1a19834bb5371c1668d93/boards/seeed_wio_tracker_L1.json)
- Variant: [`variants/nrf52840/seeed_wio_tracker_L1/variant.h`](https://github.com/meshtastic/firmware/blob/8c4900a52fca19f506f1a19834bb5371c1668d93/variants/nrf52840/seeed_wio_tracker_L1/variant.h)
  and [`variant.cpp`](https://github.com/meshtastic/firmware/blob/8c4900a52fca19f506f1a19834bb5371c1668d93/variants/nrf52840/seeed_wio_tracker_L1/variant.cpp)

Vendored into this repo as:
- `boards/seeed_wio_tracker_l1.json` — adapted from the upstream board JSON, MCU
  `nrf52840`, SoftDevice **S140 7.3.0** (`sd_fwid 0x0123`), ldscript
  `nrf52840_s140_v7.ld`, `0xFF000` bootloader settings addr, nrfutil upload @
  1200 bps touch, flash/RAM = 815104 / 248832.
  - **SoftDevice correction (W9, on hardware):** W1 initially pinned this to
    **S140 6.1.1 / `nrf52840_s140_v6.ld`** to mirror the RAK board JSON. That was
    WRONG for this board — the unit's bootloader (`INFO_UF2.TXT`) reports
    **SoftDevice S140 7.3.0**, whose application base is `0x27000` (S140 7.x is
    0x1000 larger than 6.1.1's `0x26000`). An app linked for the v6 base overlaps
    the 7.3.0 SoftDevice and will not boot. Upstream Meshtastic correctly used
    7.3.0. Fixed by reverting to 7.3.0 / v7 (see the vendored ldscript note below).
    The device booted on the first UF2 flash after this fix.
- `variants/Seeed_Wio_Tracker_L1/variant.h` + `variant.cpp` — copied verbatim
  from upstream (only the dir name changed, to match this project's
  `WisCore_RAK4631_Board`-style naming convention).

## Vendored ldscript: nrf52840_s140_v7.ld (Phase W9)

`variants/Seeed_Wio_Tracker_L1/nrf52840_s140_v7.ld` is a local copy of the
framework's `nrf52840_s140_v6.ld` with the single change `FLASH ORIGIN
0x26000 -> 0x27000` (app base for S140 7.x; RAM unchanged). The stock framework
only ships the v6 script, so we vendor v7 and select it via
`[env:wio_tracker_l1] board_build.ldscript`. `nrf52_common.ld` still resolves
from the framework linker dir (on `LIBPATH`). Required for the app to boot on
this S140-7.3.0 unit — see the SoftDevice correction note above.

## Local modifications to vendored variant.h (Phase W8)

The vendored `variant.h` does not define `LED_BUILTIN`, but Adafruit's
`InternalFileSystem` (`flash_cache.c`, pulled in by `kv_nrf52.cpp`) references
it unconditionally — the build fails with `'LED_BUILTIN' undeclared`. Added:

```c
#define LED_BUILTIN PIN_LED1
```

right after the existing `LED_GREEN`/`LED_BLUE`/`LED_STATE_ON` defines.
`PIN_LED1` (= 11 = `P1.15`) is the real Mesh status LED — safe to toggle.

**Do NOT use `LED_BLUE`** despite the compiler's "did you mean LED_BLUE?"
hint: on this board `LED_BLUE` is `PIN_LED2` (= 12 = `D12` = `P1.00`), which
is the **buzzer** pin (`BUZ_PWM`, see pin map below). Aliasing `LED_BUILTIN`
to it would make `flash_cache.c` toggle the piezo buzzer on every LittleFS
write — audible clicking on every config save.

## IMPORTANT — pin numbering convention differs from the RAK4631 variant

The RAK4631 variant uses an **identity** `g_ADigitalPinMap` (Arduino logical
pin N == absolute nRF GPIO N), so `-DPIN_*=<absolute GPIO>` in the RAK env
"just works" for both the Arduino API (`pinMode`/`digitalWrite`) and any code
that indexes `g_ADigitalPinMap[]` directly.

**The Wio Tracker L1 variant's `g_ADigitalPinMap` is NOT identity** — it's a
real translation table (e.g. logical `D4` == absolute GPIO 46). All the
`-DPIN_*` values set in `[env:wio_tracker_l1]` are **Arduino logical pin
numbers** (the `Dn` index), because that's what `pinMode`/`digitalWrite`/
RadioLib's `Module(...)` constructor expect — the core translates them via
`g_ADigitalPinMap` internally.

**This matters for Phase W4** (`sidetone_nrf52.cpp`, `-DSIDETONE_BUZZER`): the
plan's note that "`g_ADigitalPinMap[PIN_BUZZER]`... the variant maps Arduino pin
== absolute nRF GPIO" is **TRUE for the RAK4631 but FALSE for this board**. The
buzzer arm must either (a) call `g_ADigitalPinMap[PIN_BUZZER]` to translate
(logical 12 -> absolute GPIO 32) before writing `NRF_PWM0->PSEL.OUT[0]`, or (b)
just use `digitalPinToPinName()`/equivalent. Do not assume identity — verify
against `variant.cpp`'s `g_ADigitalPinMap[]` table before wiring the raw
register.

## Resolved pin map

All "logical" values are the `Dn` Arduino pin index (what goes in `-DPIN_*`);
"absolute GPIO" is `g_ADigitalPinMap[Dn]` (the real nRF52 port.pin), included
for cross-reference against the schematic and for any code that needs the raw
register/PSEL value.

| Signal | Net (schematic) | Logical (Dn) | Absolute GPIO | Port.Pin | Notes |
|--------|-----------------|--------------|---------------|----------|-------|
| LoRa NSS    | `LoRa_CS`   | D4  | 46 | P1.14 | SX1262 chip select |
| LoRa SCK    | `LoRa_SCK`  | D8  | 30 | P0.30 | = `PIN_SPI_SCK` (variant default) |
| LoRa MISO   | `LoRa_MISO` | D9  | 3  | P0.03 | = `PIN_SPI_MISO` (variant default) |
| LoRa MOSI   | `LoRa_MOSI` | D10 | 28 | P0.28 | = `PIN_SPI_MOSI` (variant default) |
| LoRa NRST   | `LoRa_RST`  | D2  | 39 | P1.07 | reset |
| LoRa BUSY   | `LoRa_BUSY` | D3  | 42 | P1.10 | |
| LoRa DIO1   | `LoRa_DIO1` | D1  | 7  | P0.07 | IRQ |
| LoRa RF switch | `LoRa_SW` | D5 | 40 | P1.08 | upstream defines `SX126X_RXEN=D5`, `SX126X_TXEN=RADIOLIB_NC`, **and** `SX126X_DIO2_AS_RF_SWITCH` simultaneously — i.e. DIO2 is the actual switch control and `LoRa_SW`/RXEN is a secondary RX-enable gate. Default assumption (keep `setDio2AsRfSwitch(true)`) should work; if RX is silent on the bench, drive D5 HIGH for RX (see W3). |
| TCXO        | DIO3        | —   | —  | —     | `SX126X_DIO3_TCXO_VOLTAGE = 1.8` — matches RAK's 1.8 V |
| OLED SDA    | `OLED_I2C_SDA` | D14 | 6 | P0.06 | = variant's default `PIN_WIRE_SDA` |
| OLED SCL    | `OLED_I2C_SCL` | D15 | 5 | P0.05 | = variant's default `PIN_WIRE_SCL`; no reset line (`U8X8_PIN_NONE`) |
| Buzzer      | `BUZ_PWM`   | D12 | 32 | P1.00 | active-high square wave; matches block-diagram's "P1.00" annotation exactly |
| Key (PIN_KEY) | `Menu_Key` | D13 | 8 | P0.08 | upstream calls this the "User Button" / `CANCEL_BUTTON_PIN`; active-LOW, pull-up. Confirmed on schematic at net `Menu_Key` -> `P0.08` |
| Mode button (PIN_MODE_BTN) | `Joystick_Press` | D29 (`TB_PRESS`) | 37 | P1.05 | **No `Rot_Key` net exists on this board** — the schematic has only `Menu_Key` + a 5-way `Joystick_*`. Substituted the joystick centre-press (`TB_PRESS`) as the mode button. |
| Battery ADC | `BAT_ADC`   | D16 (`PIN_VBAT`) | 31 | P0.31 | `ADC_MULTIPLIER = 2.0` (≈2:1 divider), `BATTERY_SENSE_RESOLUTION_BITS = 12`, `AREF_VOLTAGE = 3.6` |
| Battery gate | `BAT_ADC_CTR` | D30 (`BAT_READ`/`BAT_CTL`) | 4 | P0.04 | upstream `initVariant()` does `pinMode(BAT_READ, OUTPUT); digitalWrite(BAT_READ, HIGH)` at boot — i.e. **active-HIGH enables the divider** (opposite the always-on RAK, similar idea to the Heltec gate). Confirm against the schematic's "Power Switch" sheet on the bench. |

Spare on-board inputs not used by this port: `Joystick_Up/Down/Left/Right`
(`TB_UP`=D25/abs 36, `TB_DOWN`=D26/abs 12, `TB_LEFT`=D27/abs 11,
`TB_RIGHT`=D28/abs 35).

## Open questions this resolves from §2 of the plan

1. **RF switch:** DIO2 (kept as `setDio2AsRfSwitch(true)`), **plus** a secondary
   `LoRa_SW`/`SX126X_RXEN` GPIO (D5/abs 40) that upstream Meshtastic also
   drives. Not purely "DIO2 only" as the plan's default assumption guessed —
   may need both (see W3 note in the table above).
2. **LoRa power-enable:** confirmed **none** — no `SX126X_POWER_EN` in the
   upstream variant; always-on LDO as the plan predicted. Do not define it.
3. **TCXO voltage:** confirmed **1.8 V** (`SX126X_DIO3_TCXO_VOLTAGE = 1.8`),
   matching the RAK and the plan's safe-default guess.
4. **OLED pins == Wire defaults:** confirmed yes (`PIN_WIRE_SDA`/`SCL` are D14/
   D15, the OLED nets).
5. **SPI pins == LoRa pins:** confirmed yes (`PIN_SPI_SCK/MISO/MOSI` are D8/D9/
   D10, the `LoRa_SCK/MISO/MOSI` nets) — bare `SPI.begin()` should work.
6. **Buzzer pin:** confirmed `D12` / absolute GPIO 32 (`P1.00`), matching the
   block diagram's "P1.00" label exactly.
7. **Buttons:** `Menu_Key` = `D13`/abs GPIO 8 (`P0.08`), confirmed against the
   schematic. **`Rot_Key` does not exist** — substituted `Joystick_Press`
   (`TB_PRESS` = `D29`/abs GPIO 37) as `PIN_MODE_BTN`.
8. **Battery divider/gate:** ratio ≈ 2:1 (`ADC_MULTIPLIER = 2.0`), gate
   (`BAT_READ`/D30/abs GPIO 4) is **active-HIGH** per upstream `initVariant()`
   — confirm on the bench in W6.
