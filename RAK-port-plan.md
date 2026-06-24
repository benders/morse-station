# Multi-target port plan: Heltec V3, RAK4631 (nRF52840), Feather 32u4 eval

Status board (update as work proceeds — mirror into GitHub Issues):

| Phase | Target | State |
|-------|--------|-------|
| 0 | Shared refactor (seams, no behavior change) | `[x]` |
| 1 | Heltec WiFi LoRa 32 **V3** (ESP32-S3, no FEM) | `[x]` (build-only; **hardware validation pending — no V3 unit available**) |
| 2 | **RAK4631** (nRF52840 cross-MCU port) | `[~]` (build-only; **hardware validation pending — no RAK4631 unit available**) |
| 3 | Feather 32u4 RFM9x **written evaluation** (docs only) | `[ ]` |

This file is the implementation spec. It is written so a Sonnet sub-agent can
execute one phase at a time. **Read `AGENTS.md` first** — its rules bind every
phase: track in GitHub Issues, datasheets go in `reference/<board>/`, pin maps live
in `platformio.ini` `build_flags` as `-DPIN_*` (consumed by `src/pins.h` with
`#ifndef` fallbacks), and work conservatively/incrementally.

---

## 0. Decisions already made (do not relitigate)

- **Heltec V3 sidetone:** same I2S → MAX98357A path as the V4. `sidetone.cpp`
  is reused unchanged; the V3 env just carries the same `-DPIN_I2S_*` flags.
- **RAK4631 sidetone:** **none for now.** Ship a stubbed (silent) sidetone
  backend so it builds/runs; real audio is a later TODO.
- **Feather 32u4:** **written feasibility evaluation only** (a new docs file).
  No firmware target, no env, no code.
- **Sequencing gate:** Phase 0 must build clean **and be hardware-validated on
  Heltec V4.2, Heltec V4.3, and Cardputer ADV** before Phase 1 (V3) starts.
- **RAK build strategy:** vendor the board JSON + variant into the repo and use
  the Adafruit nRF52 BSP (see §3), matching Meshtastic/MeshCore.

---

## 1. Architecture today (what the port must preserve)

Two build targets exist, selected by a `-DDEVICE_*` define:

- `[env:heltec_v4]` → `board = heltec_wifi_lora_32_V3` (ESP32-S3 + on-board
  SX1262 + external FEM). One firmware covers **both** V4.2 (GC1109 FEM) and
  V4.3 (KCT8103L FEM) — they differ only at runtime on hardware, not as
  separate builds.
- `[env:cardputer_adv]` → `board = m5stack-stamps3` (ESP32-S3 + ES8311 codec +
  ST7789V2 LCD + TCA8418 keyboard; SX1262 on a removable cap).

Board-specific source is **self-guarded by `#ifdef DEVICE_*`** wrapping the
whole file (e.g. `display_cardputer.cpp`, `sidetone_cardputer.cpp`,
`keyboard.h`), so every `.cpp` compiles in every env but emits nothing unless
its define is set. There is no `build_src_filter` today. New targets follow the
same self-guard pattern (optionally tightened with `build_src_filter` later).

**ESP32-only API surface that blocks a non-ESP port** (counted across `src/`):
- `Preferences` / NVS — `config.cpp` (namespace `"morse"`) and the bootlog ring
  in `main.cpp` (namespace `"boot"`).
- `esp_reset_reason()`, `esp_restart()`, `esp_deep_sleep_start()`,
  `esp_sleep.h`, `esp_system.h` — `main.cpp`.
- `ESP.getEfuseMac()` — `config.cpp` (`mac_station_id()`).
- `NimBLE-Arduino` — `ble_provision.cpp` (NUS BLE-UART transport).
- ESP I2S driver (`driver/i2s.h`, FreeRTOS task) — `sidetone.cpp` (MAX98357A).
- `HSPI`, `IRAM_ATTR` — `radio.cpp`.

`radio.cpp`, `display.cpp` (U8g2 SSD1306), `morse.*`, `morsekey.*`,
`protocol.h`, `battery.cpp` (curve), and `main.cpp`'s logic are otherwise
portable.

---

## 2. RAK4631 hardware facts (authoritative for Phase 2)

Source: RAKwireless WisCore variant as used by MeshCore
(`variants/rak4631/variant.h`) and Meshtastic. **Confirm against the vendored
variant + the RAK schematic saved to `reference/rak4631/` before flashing.**

SX1262 (drive the global `SPI`, RadioLib `new Module(NSS, DIO1, NRST, BUSY, SPI)`):

| Signal | nRF52 pin | Notes |
|--------|-----------|-------|
| NSS    | 42 | chip select |
| DIO1   | 47 | IRQ |
| NRST   | 38 | reset |
| BUSY   | 46 | **confirm 46 vs 39 against vendored variant.h + schematic** |
| SCLK   | 43 | radio SPI clock |
| MISO   | 45 | |
| MOSI   | 44 | |
| **SX126X_POWER_EN** | **37** | **MUST drive HIGH before radio init** (powers the SX1262 LDO). Easy to miss → radio won't init if left low. |
| DIO2   | — | RF antenna switch → `setDio2AsRfSwitch(true)` |
| DIO3   | — | drives the TCXO → `tcxoVoltage = 1.8f` in `beginFSK()` |

No FEM (do **not** define `HAS_FEM`). No external PA: TX ceiling is the SX1262
chip max (+22 dBm).

- **Battery:** analog read on `PIN_VBAT_READ = 5` (`analogRead`), through the
  RAK19007 divider. **Divider ratio must be taken from the RAK schematic** (RAK
  uses a 1.5MΩ/1.0MΩ-class divider; confirm and calibrate). Reuse
  `battery.cpp`'s existing `volts_to_percent()` curve.
- **Display (RAK1921):** SSD1306 128×64 over I²C on the default `Wire`
  (`SDA=13, SCL=14`), **no reset line** (`U8X8_PIN_NONE`). U8g2 already supports
  it → reuse `display.cpp`'s U8g2 path with a RAK begin() that skips VEXT.
- **BLE:** Adafruit Bluefruit (`bluefruit.h`, `BLEUart`) on SoftDevice S140.
- **Default station id:** fold `NRF_FICR->DEVICEID[0]/[1]` instead of the eFuse
  MAC.
- **Reset reason:** `NRF_POWER->RESETREAS` (bitfield) instead of
  `esp_reset_reason()`.
- **Restart / power-off:** `NVIC_SystemReset()` / `sd_power_system_off()`.

---

## 3. Build-strategy comparison (Meshtastic vs MeshCore) → our choice

Both projects support the RAK4631 the same way; differences are in
filesystem/forks. Findings:

| Aspect | Meshtastic | MeshCore | **Our choice** |
|--------|-----------|----------|----------------|
| Board def | **vendored** `boards/wiscore_rak4631.json` | **vendored** `boards/rak4631.json` | **Vendor** `boards/wiscore_rak4631.json` (copy Meshtastic's verbatim) |
| `core` / `bsp` | `nRF5` / `adafruit` | `nRF5` / `adafruit` | same |
| `variant` | `WisCore_RAK4631_Board` | `WisCore_RAK4631_Board` | same — **must be on the include path** (see below) |
| ldscript | `nrf52840_s140_v6.ld` (S140 v6.1.1) | `nrf52840_s140_v6.ld` (+ `_extrafs` variants for big FS) | `nrf52840_s140_v6.ld` (no extrafs needed) |
| SoftDevice | S140 6.1.1, settings `0xFF000` | S140 6.1.1, settings `0xFF000` | same |
| Upload | `nrfutil`, 1200 bps touch, UF2 | `nrfutil`, 1200 bps touch, UF2 | same |
| max flash / RAM | 815104 / 248832 | 815104 / 235520 | 815104 / ~248832 |
| Arduino core | stock-ish | **forked** `framework-arduinoadafruitnrf52` (BLE lockup patch) via `platform_packages` | **stock Adafruit core first**; fall back to the meshcore-dev fork only if we hit the rapid connect/disconnect lockup |
| Radio lib | RadioLib | RadioLib 7.6 + `RADIOLIB_GODMODE` + many `RADIOLIB_EXCLUDE_*` | RadioLib 7.x; add `RADIOLIB_EXCLUDE_*` to shrink flash |
| Filesystem | Adafruit InternalFS / LittleFS | **CustomLFS** (`oltaco/CustomLFS`) + `-DEXTRAFS=1` + extrafs ldscript | **bundled `Adafruit_LittleFS` + `InternalFileSystem`** (simplest; our config is tiny) |
| Pin map | named macros in variant | `P_LORA_*` macros in `variant.h` | `-DPIN_*` build flags from the variant numbers (§2) |
| Src selection | heavy `build_src_filter` per variant | heavy `build_src_filter` per variant | keep self-guard `#ifdef`; add `build_src_filter` only if needed |

**The variant gotcha:** the board JSON names `variant:
WisCore_RAK4631_Board`, but the **stock** Adafruit nRF52 core package does *not*
ship that variant — RAKwireless does. Two ways to satisfy it (pick in Phase 2):

- **(A) Vendor the variant dir** into the repo (e.g.
  `variants/WisCore_RAK4631_Board/` with `variant.h`/`variant.cpp` from
  RAKwireless's BSP) and point PlatformIO at it via
  `board_build.variants_dir = variants` (or `-I` + `build_src_filter`). Most
  self-contained; matches "vendor everything." **Preferred.**
- **(B) `platform_packages`** to pull a core fork that already contains the
  variant (RAKwireless's `RAK-nRF52-Arduino`, or meshcore-dev's fork). Less in
  our tree, but pins us to someone else's fork.

Save the chosen approach + the variant source provenance into
`reference/rak4631/README.md`.

---

## Phase 0 — Shared refactor (seams only, **zero behavior change**)

Goal: introduce platform/persistence seams so the nRF52 port has clean plug
points, **without changing how the two existing ESP32 builds behave**. This is
the gate the rest depends on.

> Sub-agent constraints for Phase 0: **do not** add the V3 or RAK envs, **do
> not** touch radio/display/sidetone/BLE logic, **do not** change runtime
> behavior. Only the refactors below. The ESP32 code paths must compile to the
> same flags and behave identically.

### P0.1 — `platformio.ini`: stop ESP32-only flags leaking to future targets

Today `[env].build_flags` holds ESP32-S3-only USB flags that both ESP envs
inherit via `${env.build_flags}`. Restructure so those flags live in an
ESP32-only anchor, and the shared `[env]` keeps only cross-platform settings.

- Keep in `[env]`: `extra_scripts = pre:scripts/git_rev.py`, `monitor_speed`,
  `upload_speed`, `upload_port`/`monitor_port` globs. **Remove** `platform`,
  `framework`, and the `-DARDUINO_USB_*` flags from `[env]` (these are
  ESP32-only).
- Add `[esp32_base]` (a plain section, not `env:`):
  ```
  [esp32_base]
  platform = espressif32
  framework = arduino
  build_flags =
      -DARDUINO_USB_CDC_ON_BOOT=1
      -DARDUINO_USB_MODE=1
      ; (keep the DEBUG_PANIC_CMD comment block here)
  ```
- `[env:heltec_v4]` and `[env:cardputer_adv]`: add `extends = esp32_base` and
  change their `build_flags` to start with `${esp32_base.build_flags}` instead
  of `${env.build_flags}`.
- **Acceptance:** the *effective* compiler flags for both envs are unchanged.
  Verify with `pio run -t envdump` or by diffing a verbose build's flag list
  before/after; both envs must still build.

### P0.2 — `src/platform.h` + `src/platform_esp32.cpp`: chip-call seam

New header `src/platform.h`:
```cpp
#pragma once
#include <stdint.h>
namespace platform {
void     restart();          // was esp_restart()
void     system_off();       // was esp_deep_sleep_start() (no wake source)
int      reset_reason();     // was (int)esp_reset_reason(); values must match
                             //   the existing reset_reason_label() table
uint8_t  unique_id_byte();   // folded device id for the default station id
}
```
`src/platform_esp32.cpp` (guard with `#if defined(DEVICE_HELTEC_V4) ||
defined(DEVICE_CARDPUTER_ADV)` so a future nRF52 file can supply the same
symbols):
- `restart()` → `esp_restart()`
- `system_off()` → `esp_deep_sleep_start()`
- `reset_reason()` → `(int)esp_reset_reason()` (**must keep the same integer
  values** so `reset_reason_label()` in `main.cpp` stays correct)
- `unique_id_byte()` → the current `mac_station_id()` fold of
  `ESP.getEfuseMac()`, returning the 1..254 byte (move that logic here).

Wire-in:
- `main.cpp`: `#include "platform.h"`; drop `#include <esp_sleep.h>` and
  `#include <esp_system.h>`; replace `esp_reset_reason()` →
  `platform::reset_reason()`, `esp_restart()` → `platform::restart()`,
  `esp_deep_sleep_start()` → `platform::system_off()`. Leave
  `reset_reason_label()` where it is (ESP labels; Phase 2 adds nRF52 labels).
- `config.cpp`: `mac_station_id()` becomes a thin call to
  `platform::unique_id_byte()` (or delete it and call platform directly).
  Remove the `ESP.getEfuseMac()` use from `config.cpp`.
- **Acceptance:** boot banner still prints the same `# boot #N reason=...`
  output on hardware; station id unchanged on each unit.

### P0.3 — `src/kv.h` + `src/kv_esp32.cpp`: persistence seam

Wrap the namespaced key/value store so `config.cpp` and the `main.cpp` bootlog
stop calling `Preferences` directly. Header `src/kv.h`:
```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace kv {
class Store {
public:
  bool begin(const char* ns, bool read_only);
  void end();
  bool     isKey(const char* k);
  uint8_t  getUChar(const char* k, uint8_t def);
  void     putUChar(const char* k, uint8_t v);
  uint32_t getUInt(const char* k, uint32_t def);
  void     putUInt(const char* k, uint32_t v);
  bool     getBool(const char* k, bool def);
  void     putBool(const char* k, bool v);
  size_t   getString(const char* k, char* out, size_t cap);
  void     putString(const char* k, const char* v);
  size_t   getBytes(const char* k, void* out, size_t cap);
  void     putBytes(const char* k, const void* v, size_t len);
  void     remove(const char* k);
};
}
```
`src/kv_esp32.cpp` (same ESP guard): a 1:1 wrapper over a `Preferences`
member — every method forwards to the matching `Preferences` call with
identical semantics (this is a passthrough, so NVS layout and behavior are
unchanged).

Wire-in:
- `config.cpp`: replace the file-scope `Preferences prefs;` with `kv::Store
  prefs;`; `#include "kv.h"` instead of `<Preferences.h>`; all call sites stay
  textually identical (same method names). Namespace string `"morse"` unchanged.
- `main.cpp`: the bootlog ring (`bootlog_record`/`bootlog_dump`/`bootlog_clear`)
  swaps `Preferences bl;` → `kv::Store bl;`; `#include "kv.h"`; drop
  `<Preferences.h>`. Namespace `"boot"`, keys `cnt`/`log`/`lh` unchanged.
- **Acceptance:** existing NVS contents still read back (callsign, msg, wpm,
  boot mode, volume, mute, bootlog history survive a reflash that preserves
  NVS); `show` and `bootlog` print the same as before.

### P0.4 — Phase 0 verification (the gate)

1. **Build both existing envs clean:**
   `~/.platformio/penv/bin/pio run -e heltec_v4` and `-e cardputer_adv` — both
   must succeed with no new warnings attributable to the refactor.
2. **Flag-diff:** confirm P0.1 didn't change effective flags (envdump/verbose).
3. **Hand off for hardware validation (USER GATE — do not start Phase 1 until
   the user confirms):** flash and smoke-test on **all three units** — Heltec
   V4.2, Heltec V4.3, Cardputer ADV — checking: boot banner + reason line,
   `show` (config intact), `bootlog`, fox keys / hunter copies, sidetone, mute,
   battery readout. Use `scripts/devices.sh` to map ports↔stations and the
   `--no-stub` flash fallback if the stub upload fails after port churn.
4. Update GitHub Issues and this file's status board to `[x]` for Phase 0 only.

Files touched in Phase 0: `platformio.ini`, new `src/platform.h`,
`src/platform_esp32.cpp`, new `src/kv.h`, `src/kv_esp32.cpp`, edits to
`src/main.cpp` and `src/config.cpp`. No new envs, no logic changes.

---

## Phase 1 — Heltec WiFi LoRa 32 V3 (after Phase 0 sign-off)

The V3 is the V4 minus the FEM, with the opposite battery-ADC gate polarity.
Everything else (radio, U8g2 display, NimBLE, config, I2S/MAX98357A sidetone)
is reused.

### P1.1 — `platformio.ini`: `[env:heltec_v3]`
```
[env:heltec_v3]
extends = esp32_base
board = heltec_wifi_lora_32_V3
lib_deps =
    jgromes/RadioLib@^7.0.0
    olikraus/U8g2@^2.35.30
    h2zero/NimBLE-Arduino@^1.4.2
build_flags =
    ${esp32_base.build_flags}
    -DDEVICE_HELTEC_V3
    -DPIN_I2S_BCLK=48
    -DPIN_I2S_LRCLK=4
    -DPIN_I2S_DIN=47
```
(Same I2S pins as V4 per the audio decision. Adjust if the V3 board wires the
MAX98357A elsewhere — confirm on hardware.)

### P1.2 — `src/pins.h`: add a `DEVICE_HELTEC_V3` branch
- Radio pins identical to V4 (NSS=8, SCK=9, MOSI=10, MISO=11, NRST=12, BUSY=13,
  DIO1=14).
- OLED 17/18/21, `PIN_VEXT_CTRL=36`, key/mode as V4.
- **Do not define `HAS_FEM`** and do not declare the `PIN_FEM_*` constants.
- Battery: `PIN_VBAT_ADC=1`, `PIN_VBAT_CTRL=37`, but mark the gate **active-LOW**
  (V3 convention) — see P1.3.
- Same `PIN_I2S_*` `#ifndef` fallbacks as the V4 block.

### P1.3 — `src/battery.cpp`: parameterize gate polarity
- Add a compile-time `BATT_GATE_ACTIVE_HIGH` (V4 = 1, V3 = 0), defined per board
  in `pins.h` or the env. In `read_volts()`/`begin()`, drive the gate to its
  "connect" level (HIGH for V4, LOW for V3) for the sample and park it at the
  "disconnect" level otherwise. Keep the V4 path byte-identical when the macro
  is 1.

### P1.4 — Generalize the shared U8g2 / I2S guards to include V3
- `display.cpp` and `sidetone.cpp` currently gate the Heltec path with
  `#ifndef DEVICE_CARDPUTER_ADV`, which already includes V3 — but make the
  intent explicit: `#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3)`
  for the U8g2/I2S bodies. `radio.cpp`'s `HAS_FEM` blocks already compile out
  when `HAS_FEM` is undefined.
- `platform_esp32.cpp` / `kv_esp32.cpp` guards: add `|| defined(DEVICE_HELTEC_V3)`.

### P1.5 — Verify
- Build `-e heltec_v3`. Hardware-validate on a V3 unit: radio TX/RX vs an
  existing hunter, sidetone, battery %, BLE/serial console, bootlog.
- Update GitHub Issues + status board.

---

## Phase 2 — RAK4631 (nRF52840 cross-MCU port)

Prereq: Phases 0 & 1 done (the platform/kv/display/sidetone seams exist and the
ESP path is proven). Implement in this order; each step should compile before
the next.

### P2.1 — Vendor board + variant + references
- Save `boards/wiscore_rak4631.json` (copy Meshtastic's; see §3 fields).
- Vendor the variant (approach A in §3): `variants/WisCore_RAK4631_Board/`
  (`variant.h`/`variant.cpp` from RAKwireless's BSP) and set
  `board_build.variants_dir = variants` in the env. Record provenance in
  `reference/rak4631/README.md`.
- Save to `reference/rak4631/`: RAK4631 (WisCore) datasheet, RAK19007 base-board
  schematic, RAK1921 OLED docs, and the SX1262 wiring page. Confirm the §2 pin
  map (esp. BUSY 46 vs 39, the VBAT divider ratio, and `SX126X_POWER_EN`).

### P2.2 — `platformio.ini`: `[env:rak4631]`
```
[nrf52_base]            ; optional shared anchor if more nRF52 boards come later
platform = nordicnrf52
framework = arduino
board_build.variants_dir = variants

[env:rak4631]
extends = nrf52_base
board = wiscore_rak4631
lib_deps =
    jgromes/RadioLib@^7.0.0
    olikraus/U8g2@^2.35.30
build_flags =
    -DDEVICE_RAK4631
    -DPIN_NSS=42 -DPIN_DIO1=47 -DPIN_NRST=38 -DPIN_BUSY=46
    -DPIN_SCK=43 -DPIN_MISO=45 -DPIN_MOSI=44
    -DSX126X_POWER_EN=37
    ; flash shrink (optional, mirror MeshCore):
    -DRADIOLIB_EXCLUDE_CC1101 -DRADIOLIB_EXCLUDE_RF69 -DRADIOLIB_EXCLUDE_SX128X
    -DRADIOLIB_EXCLUDE_SX127X -DRADIOLIB_EXCLUDE_SI443X
upload_protocol = nrfutil
```
(BLE comes from the Adafruit core, not `lib_deps`. Keep the `extra_scripts`
git-rev hook from `[env]`.) Note: `[env]`'s `upload_port`/`monitor_port` globs
are fine; the nRF52 uses 1200 bps touch + nrfutil from the board JSON.

### P2.3 — `src/pins.h`: `DEVICE_RAK4631` branch
- Consume the radio `-DPIN_*` flags via `#ifndef` fallbacks (mirroring §2).
- Declare `SX126X_POWER_EN` fallback.
- RAK1921 OLED: no RST (`PIN_OLED_RST = U8X8_PIN_NONE`); I²C on default Wire
  (SDA=13/SCL=14 are the core defaults — no explicit pins needed by U8g2 HW-I2C).
- No FEM, no VEXT. `PIN_SIDETONE = -1`. `PIN_VBAT_READ = 5`. Define
  `PIN_KEY`/`PIN_MODE_BTN` for a RAK19007 button (confirm a usable GPIO; the
  WisBlock user button or an IO-slot pin).

### P2.4 — `src/platform_nrf52.cpp` (implements `platform.h`)
- `restart()` → `NVIC_SystemReset()`.
- `system_off()` → `sd_power_system_off()` (fallback `NRF_POWER->SYSTEMOFF = 1`).
- `reset_reason()` → read+clear `NRF_POWER->RESETREAS`, map bits to a small enum.
  **Also provide an nRF52 `reset_reason_label()`** (different reasons than ESP);
  refactor `main.cpp` so the label lookup is platform-provided (move the ESP
  table into `platform_esp32.cpp`, add the nRF52 table here, expose
  `platform::reset_reason_label(int)`). Update `main.cpp` to call it.
- `unique_id_byte()` → fold `NRF_FICR->DEVICEID[0] ^ DEVICEID[1]` into 1..254.
- Guard: `#if defined(DEVICE_RAK4631)`.

### P2.5 — `src/kv_nrf52.cpp` (implements `kv.h`)
- Back with `Adafruit_LittleFS` + `InternalFileSystem` (bundled in the Adafruit
  core; no extra `lib_deps`). Model each `Store` namespace as a directory or a
  single packed file keyed by name; implement the same get/put/isKey/remove
  semantics as the ESP wrapper. Simplest robust approach: one file per
  namespace holding a small serialized key→value map; load on `begin`, flush on
  `put`/`end`. Keep keys/values byte-compatible with what `config.cpp` writes.
- Guard: `#if defined(DEVICE_RAK4631)`.

### P2.6 — `src/radio.cpp`: nRF52 path
- Replace `SPIClass radioSpi(HSPI)` with the global `SPI` on nRF52 (`#if
  defined(DEVICE_RAK4631)` → `SX1262 chip = new Module(PIN_NSS, PIN_DIO1,
  PIN_NRST, PIN_BUSY, SPI);` and `SPI.begin();`). Keep the ESP `HSPI` path under
  its guard.
- Remove/neutralize `IRAM_ATTR` on nRF52 (define it empty or drop it for the
  ISR — nRF52 has no IRAM attribute).
- In `init()`: **drive `SX126X_POWER_EN` HIGH** (`pinMode` OUTPUT; `digitalWrite`
  HIGH; `delay(10)`) before `beginFSK()`. Pass `tcxoVoltage = 1.8f` and call
  `setDio2AsRfSwitch(true)` (already present). `TCXO_V` branch: add a
  `DEVICE_RAK4631` case = 1.8f. No FEM branch (HAS_FEM undefined).

### P2.7 — `src/ble_provision_nrf52.cpp` (new; implements `ble_provision.h`)
- Use Adafruit Bluefruit `BLEUart`. `begin(adv_name, handler)`:
  `Bluefruit.begin()`, set name, add the BLEUart service, start advertising
  (NUS UUID is what `BLEUart` exposes). `process()`: drain `bleuart.available()`
  / `bleuart.read()` into the line assembler on the main loop, dispatch each
  full line through the same `handle_setup_command` with a `Print` sink that
  writes back via `bleuart.write()` (chunk to the negotiated MTU, default 20).
  `connected()` / `stop()` map to Bluefruit equivalents. No FreeRTOS queue
  needed (we already poll from the main loop).
- Guard `ble_provision.cpp` (NimBLE) to ESP-only: wrap its body in
  `#if defined(DEVICE_HELTEC_V4) || defined(DEVICE_HELTEC_V3) ||
  defined(DEVICE_CARDPUTER_ADV)`; guard the new file with `#if
  defined(DEVICE_RAK4631)`.

### P2.8 — `src/sidetone_nrf52.cpp` (new; stub, silent)
- Implement every `sidetone.h` symbol as a no-op (with state stored for
  `set_mute`/`set_level` so `show` stays sane). Add a `// TODO: tone()-based
  piezo path`. Guard `sidetone.cpp` (I2S) to `#if defined(DEVICE_HELTEC_V4) ||
  defined(DEVICE_HELTEC_V3)`; guard the new file with `#if
  defined(DEVICE_RAK4631)`. (`sidetone_cardputer.cpp` already self-guards.)

### P2.9 — `src/battery.cpp`: nRF52 branch
- Add `#elif defined(DEVICE_RAK4631)` branch: `analogRead(PIN_VBAT_READ)` with
  the RAK divider ratio (from the schematic) → volts → reuse `volts_to_percent`.
  `charging()` → best-effort (false unless a sense line is wired).

### P2.10 — `src/display.cpp`: include RAK in the U8g2 path
- Extend the guard to `#if defined(DEVICE_HELTEC_V4) ||
  defined(DEVICE_HELTEC_V3) || defined(DEVICE_RAK4631)`.
- Board-specific `begin()`: under `DEVICE_RAK4631`, **skip VEXT**, and construct
  the SSD1306 with `U8X8_PIN_NONE` reset on HW I²C (default Wire). Use the
  `U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE)` constructor for
  RAK; keep the Heltec constructor (with RST/SCL/SDA) under the Heltec defines.

### P2.11 — `main.cpp` residual nRF52 fit
- Already de-ESP'd by Phase 0 (uses `platform::`/`kv::`). The remaining
  `#ifdef DEVICE_CARDPUTER_ADV` blocks simply don't apply. The `hibernate()`
  helper's FEM/VEXT power-down is under `#ifdef HAS_FEM` → compiles out on RAK;
  `platform::system_off()` does the nRF52 sleep.

### P2.12 — Build, fit, flash, verify
- `pio run -e rak4631`; confirm it fits under 815104 flash / RAM budget (add
  `RADIOLIB_EXCLUDE_*` as needed). Flash via UF2 / `adafruit-nrfutil` (1200 bps
  touch). Validate: boot banner, `show`/`bootlog` (LittleFS persistence), radio
  TX/RX against a Heltec hunter on 905.0 MHz, OLED screens, BLE-UART console
  (nRF Connect), battery %. Sidetone silent (expected).
- Update GitHub Issues + status board.

---

## Phase 3 — Feather 32u4 RFM9x written evaluation (docs only)

Deliverable: `docs/eval-feather-32u4.md`. **No env, no code.** Content outline:

- **Verdict (lead with it):** the full unified firmware **does not fit / is not
  recommended** on the ATmega32u4. A heavily stripped, serial-only,
  single-role (fox-only *or* hunter-only) build *might* fit but is a different
  program, not a port.
- **Why:**
  - **No BLE radio at all** on the 32u4 → the entire `ble_provision` transport
    is removed; config becomes serial-only.
  - **2.5 KB SRAM is the wall:** U8g2 full-frame SSD1306 buffer (~1 KB) +
    RadioLib state + rolling text/dit-dah buffers + config cache won't
    co-exist. Needs U8g2 **page mode** and trimmed buffers; even then tight.
  - **32 KB flash:** RadioLib + U8g2 is plausible only with BLE gone and
    `RADIOLIB_EXCLUDE_*` aggressive trimming.
  - **RFM9x = SX1276, not SX1262:** different RadioLib class (`RFM95`/`SX1276`).
    RadioLib's SX127x **does** support GFSK, so the on-air `proto::` packets
    (4.8 kbps GFSK, sync word) can still interoperate — but DIO mapping, RF
    switch, and TCXO handling differ, needing a separate `radio.cpp` backend.
  - **Persistence:** AVR `EEPROM` (1 KB) instead of NVS/LittleFS.
  - **Sidetone:** `tone()` on a GPIO (piezo).
  - **No FreeRTOS, 8-bit, 8/16 MHz** — the I2S sidetone and any task-based code
    are out.
- **If pursued anyway:** sketch the minimum viable shape (serial-only fox-only,
  U8g2 page mode or no display, EEPROM config, SX1276 GFSK matching `proto.h`)
  and call out the RAM/flash risk explicitly.
- Cross-link from GitHub Issues and (optionally) `README.md`.

---

## Sequencing & gates (summary)

1. **Phase 0** → build both ESP envs + **hardware sign-off on V4.2 / V4.3 /
   Cardputer ADV** → *user confirms* → unblock Phase 1.
2. **Phase 1** (V3) → build + hardware-validate on a V3 → unblock Phase 2.
3. **Phase 2** (RAK4631) → build/fit + hardware-validate.
4. **Phase 3** (32u4 doc) → independent; can be written any time.

Each phase: keep changes minimal and reversible, update GitHub Issues (`[~]`→`[x]`),
and save any datasheets pulled into `reference/`.
