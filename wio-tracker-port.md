# Port plan: Seeed Wio Tracker L1 Pro (nRF52840 + SX1262)

Status board (update as work proceeds — mirror into `TODO.md`):

| Phase | Scope | State |
|-------|-------|-------|
| W1 | Vendor board JSON + variant + references; add `[env:wio_tracker_l1]` | `[x]` |
| W2 | `src/pins.h` — `DEVICE_WIO_TRACKER_L1` branch | `[x]` |
| W3 | `src/radio.cpp` — share the nRF52/global-SPI SX1262 path; RF switch + TCXO | `[x]` |
| W4 | `src/sidetone_nrf52.cpp` — new passive-buzzer backend (`-DSIDETONE_BUZZER`) | `[x]` |
| W5 | `src/pins.h` + buttons — wire an on-board button as the Morse key + mode btn | `[x]` (no code — satisfied by W2; verified against variant.h) |
| W6 | `src/battery.cpp` — gated nRF52 analog read | `[x]` |
| W7 | `src/display.cpp` — include Wio in the U8g2 OLED path | `[x]` |
| W8 | Extend `platform_nrf52`/`kv_nrf52`/`ble_provision_nrf52`/`sidetone` guards | `[x]` |
| W9 | Build, fit, flash, hardware-validate | `[~]` (build/fit ✅; **boots on real hardware** ✅ banner/config/BLE/OLED/battery; 2 HW bugs found+fixed: buzzer tick, SH1106 panel. On-air radio + buttons still to test) |

This file is the implementation spec, written so a **Sonnet sub-agent can execute
one phase at a time**. **Read `AGENTS.md` first** — its rules bind every phase:
track in `TODO.md`, datasheets/schematics go in `reference/<board>/`, pin maps
live in `platformio.ini` `build_flags` as `-DPIN_*` (consumed by `src/pins.h`
with `#ifndef` fallbacks), and work conservatively/incrementally (smallest change
that moves one item forward; prove it builds before the next).

The RAK4631 port (`RAK-port-plan.md`, Phase 2) is the **direct precedent** — read
it. This board reuses almost all of it.

---

## 0. The big picture (why this port is small)

The Wio Tracker L1 Pro is, electrically, a **sibling of the RAK4631**:

- **MCU:** nRF52840 (same Adafruit nRF5 core / SoftDevice S140, same `nordicnrf52`
  platform, same Bluefruit BLE, same LittleFS persistence).
- **Radio:** a **Seeed `Wio-SX1262` module** — i.e. a plain **SX1262**, the *same
  chip RadioLib already drives on every other target*. (The product block diagram
  is fuzzy and looks like an LR1110, but schematic sheet 4/6 `Module.kicad_sch`
  clearly labels `U10 = Wio-SX1262` with the standard NSS/SCK/MISO/MOSI/RST/BUSY/
  DIO1 pinout. **It is an SX1262, not an LR1110.**)
- **Display:** 0.96″ **SSD1306 128×64 OLED over I²C** (block diagram + sheet 5/6
  "OLED Interface") — the same U8g2 path the Heltec/RAK builds use. (There is
  *also* a 250×122 E-ink panel and an L76K GNSS on the board; **both are out of
  scope** for this port — we do not drive them.)

So everything the RAK4631 port already built is reused **as-is or with a one-line
guard widening**:

| Concern | RAK4631 file | Wio reuse |
|---------|--------------|-----------|
| `platform::` (restart/system_off/reset_reason/unique_id) | `platform_nrf52.cpp` | **as-is**, widen guard |
| `kv::` LittleFS persistence | `kv_nrf52.cpp` | **as-is**, widen guard |
| BLE-UART (Bluefruit) console | `ble_provision_nrf52.cpp` | **as-is**, widen guard |
| SX1262 over global `SPI` | `radio.cpp` (`#if DEVICE_RAK4631`) | widen guard (W3) |
| U8g2 SSD1306 OLED | `display.cpp` (`#if DEVICE_RAK4631`) | widen guard (W7) |
| Morse logic / protocol / config / main loop | shared | **as-is** |

**The only genuinely new code is three things:**

1. **Sidetone = passive piezo buzzer.** The board has a built-in passive buzzer
   (sheet 5/6 "Buzzer": `BUZ1`, a 2700 Hz transistor-driven piezo on net
   `BUZ_PWM`). No I2S, no MAX98357A, no PAM8403. This needs a *new, simpler*
   sidetone backend: a square wave at the sidetone frequency on one GPIO. **This
   is a finished, sealed unit — we cannot add or rewire any hardware**, so the
   built-in buzzer is the only option and the design must accept its quality.
2. **Key = an existing on-board button.** No telegraph-key jack. Map one of the
   on-board buttons (Menu / rotary / 5-way joystick) to `PIN_KEY` for live-key
   TX, and another to `PIN_MODE_BTN` for the boot menu.
3. **Battery = a gated analog read** (the board gates its VBAT divider with a
   MOSFET, like the Heltec — unlike the RAK which reads continuously).

---

## 1. Decisions already made (do not relitigate)

- **Device define:** `DEVICE_WIO_TRACKER_L1`. **Env:** `[env:wio_tracker_l1]`,
  `extends = nrf52_base` (already in `platformio.ini`).
- **Radio:** SX1262 via the **existing** nRF52 global-`SPI` path in `radio.cpp`.
  Do **not** write a new radio backend. Just widen the RAK guard.
- **Sidetone:** **passive buzzer**, square wave via the nRF52 PWM peripheral,
  selected with a new `-DSIDETONE_BUZZER` build-time backend inside
  `sidetone_nrf52.cpp`. Tone quality is explicitly accepted as "good enough"
  (single piezo, no envelope, coarse/▒no volume control). **No new hardware.**
- **Display:** reuse the U8g2 SSD1306 HW-I²C path. E-ink + GNSS are **not** ported.
- **No FEM / no external PA:** TX ceiling is the SX1262 chip max (+22 dBm). Do
  **not** define `HAS_FEM`.
- **Variant strategy:** **vendor** the Seeed/Meshtastic variant dir into
  `variants/` (same "Option A" the RAK port used) — see §3.
- **Hardware:** the unit is in a finished case. Validation is button-press +
  serial/BLE console + over-the-air against an existing hunter; no probing of
  internal headers.

---

## 2. Hardware facts (authoritative pin map — CONFIRM against the vendored variant.h)

**Provenance:** schematic PDF in `reference/wio-tracker-l1-pro/`
(`Wio_Tracker_L1_Pro_SCH_PDF.pdf`, "Meshtastic Repeater_Handle" rev v1.3) and the
block diagram (`L1_Diagram.png`). The schematic's *block-diagram* GPIO annotations
are low-resolution and internally inconsistent (the same "Gpio14" label appears
on two different nets), so **they are not trustworthy enough to flash from**. The
**authoritative source is the Seeed/Meshtastic `variant.h`** for this board (see
§3) — every pin below MUST be reconciled against it before any hardware step.
This board *is* an upstream Meshtastic target, so a maintained `variant.h` with
correct pin macros exists; use it.

### SX1262 (drive the global `SPI`, RadioLib `new Module(NSS, DIO1, NRST, BUSY, SPI)`)

| Signal | Net name | Notes |
|--------|----------|-------|
| NSS    | `LoRa_CS`   | chip select |
| SCK    | `LoRa_SCK`  | radio SPI clock |
| MISO   | `LoRa_MISO` | |
| MOSI   | `LoRa_MOSI` | |
| NRST   | `LoRa_RST`  | reset |
| BUSY   | `LoRa_BUSY` | |
| DIO1   | `LoRa_DIO1` | IRQ |
| RF switch | `LoRa_SW` and/or DIO2 | **see note ↓** |
| TCXO   | DIO3 | Wio-SX1262 module has a TCXO; `tcxoVoltage = 1.8f` (match RAK) — confirm |

- **RF switch (the one thing to get right):** the module exposes a `LoRa_SW` net
  *and* the SX1262's DIO2. Determine from the variant.h / Meshtastic config which
  controls the antenna switch:
  - If DIO2 drives it (common for Wio-SX1262): keep `setDio2AsRfSwitch(true)`
    (already in `radio.cpp`) — **nothing else to do.**
  - If `LoRa_SW` is a separate nRF GPIO: drive it, or register it via RadioLib
    `setRfSwitchPins(rxEn, txEn)`. Add this only if the variant/Meshtastic shows
    a discrete switch GPIO. **Default assumption: DIO2, matching RAK.**
- **LoRa power-enable:** the `LoRa_3V3` rail is an **always-on LDO** (sheet 3/6
  `U6`), so — unlike the RAK's `SX126X_POWER_EN` — there is most likely **no
  software power-enable** to drive. **Do NOT define `SX126X_POWER_EN`** unless the
  variant.h shows a discrete LoRa-enable GPIO. (radio.cpp's POWER_EN block stays
  RAK-only; see W3.)
- **SPI pins:** `radio.cpp` calls `SPI.begin()` and relies on the **variant's**
  default `PIN_SPI_SCK/MISO/MOSI` being the LoRa pins. Confirm the vendored
  `variant.h` maps the SPI peripheral onto the `LoRa_*` pins; if not, re-pin with
  `SPI.begin()`/`SPI.setPins(...)` in the nRF52 init (see W3).

### OLED — SSD1306 128×64 over I²C

- Nets `OLED_I2C_SCL` / `OLED_I2C_SDA`. **No reset line** (`U8X8_PIN_NONE`).
- Reuse the RAK display path: `U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0,
  U8X8_PIN_NONE)`. Ensure the variant maps `PIN_WIRE_SDA/SCL` to the OLED I²C
  pins (or call `Wire.setPins(sda, scl)` before `Wire.begin()` — see W7).
- The OLED rail (`BUZ_3V3`, sheet 3/6 `U8`) is also always-on → **no VEXT gate**.

### Buzzer (passive piezo)

- Net `BUZ_PWM` → `Q13` (NPN) → piezo `BUZ1` (resonant ≈ 2700 Hz), powered from
  `BUZ_3V3`. **Active-high**: drive `BUZ_PWM` with a square wave to sound it; idle
  LOW = silent. Block diagram labels it `P1.00` — **confirm the pin from variant.h.**
- One GPIO, output-only. Define `-DPIN_BUZZER=<n>` in the env.

### Buttons (pick the key + mode button from these)

All are active-LOW to GND with pull-ups present (sheet 6/6 + 7 show 100 kΩ
pull-ups + TVS), so `INPUT_PULLUP` + `LOW == pressed` matches `MorseKey` and the
existing mode-button logic in `main.cpp` exactly.

| Net | Candidate role | Notes |
|-----|----------------|-------|
| `Menu_Key`        | **`PIN_KEY`** (live-key telegraph) | big front button, good for keying |
| `Rot_Key` (`Rot_key`) | **`PIN_MODE_BTN`** (boot-menu nav) | the other top button |
| `Joystick_Press`/`_Up`/`_Down`/`_Left`/`_Right` | spare | 5-way; not required |

**Decision for this port:** `PIN_KEY = Menu_Key`, `PIN_MODE_BTN = Rot_Key`. Get
the exact GPIO numbers from variant.h (the Meshtastic variant names these as
button macros). If the variant only breaks out one usable button, put the key on
it and lean on `run_menu()`'s 5 s idle auto-select for the boot menu (same
fallback the RAK branch documents).

### Battery

- Net `BAT_ADC`, gated by `BAT_ADC_CTR` (sheet 3/6 "Power Switch": a MOSFET
  enables the divider only while sampling — same idea as the Heltec gate, **not**
  the RAK's always-on divider). Drive `BAT_ADC_CTR` to its connect level, sample
  `BAT_ADC`, then park it.
- **Divider ratio + gate polarity: CONFIRM from the schematic** (the sense network
  looks like a ~1:1 / ratio≈2 divider; verify R values and which level of
  `BAT_ADC_CTR` connects). Calibrate against a meter once on the bench.
- Reuse `battery.cpp`'s `volts_to_percent()` curve unchanged.

### Out of scope (do not wire up)

L76K GNSS (`GNSS_*`), the 250×122 E-ink panel (`Ink_*`), Grove port, solar/charge
status LEDs, Mesh LED (optional: could mirror TX state, but not required).

---

## 3. Variant + board JSON (vendor it — Option A, same as RAK)

The `[env:wio_tracker_l1]` board JSON will name a `variant` that the **stock**
Adafruit nRF52 core does not ship. Vendor it, exactly like the RAK port did with
`WisCore_RAK4631_Board`:

1. **Board JSON:** add `boards/seeed_wio_tracker_l1.json`. Start from Meshtastic's
   board file for this target (or copy `boards/wiscore_rak4631.json` and edit:
   MCU `nrf52840`, SoftDevice **S140 6.1.1**, ldscript `nrf52840_s140_v6.ld`,
   `0xFF000` settings, `nrfutil` upload @ 1200 bps touch, `variant` = the vendored
   dir name, flash/RAM = 815104 / 248832). Cross-check every field against the
   upstream Meshtastic board JSON for the Wio Tracker L1.
2. **Variant dir:** vendor `variants/Seeed_Wio_Tracker_L1/` containing
   `variant.h` + `variant.cpp` from the **Seeed/Meshtastic nRF52 BSP** for this
   board. `platformio.ini` already sets `board_build.variants_dir = variants`
   (in `[nrf52_base]`), so the dir name just has to match the board JSON's
   `variant`.
3. **Provenance:** write `reference/wio-tracker-l1-pro/README.md` recording: the
   product page (https://www.seeedstudio.com/Wio-Tracker-L1-Pro-p-6454.html), the
   exact upstream repo + commit the `variant.*` and board JSON came from, and the
   resolved pin map (so future bring-up doesn't re-derive it from the fuzzy block
   diagram). The schematic PDF + block diagram already live in that folder.

> **The variant.h is the source of truth for every pin in §2.** Phase W2 must
> reconcile the §2 table against it. If a `variant.h` for the *L1 Pro* specifically
> doesn't exist yet upstream but the plain *L1* does, start from the L1's and
> diff against this board's schematic.

---

## Phase W1 — Vendor board + variant + references; add the env

1. Do §3 (board JSON, variant dir, `reference/.../README.md`).
2. Add to `platformio.ini`:

```ini
[env:wio_tracker_l1]
; Seeed Wio Tracker L1 Pro — nRF52840 + Wio-SX1262 (SX1262, no FEM), SSD1306
; 128x64 OLED (I2C), built-in passive piezo buzzer for sidetone, on-board
; buttons for the Morse key. Sibling of [env:rak4631]; reuses the nRF52 backend
; (platform_nrf52 / kv_nrf52 / ble_provision_nrf52) and the global-SPI SX1262
; path in radio.cpp. See wio-tracker-port.md.
;
; Sidetone is the built-in passive buzzer: a square wave on PIN_BUZZER (-> NPN
; -> piezo). Selected by -DSIDETONE_BUZZER (see src/sidetone_nrf52.cpp).
;
; PIN MAP IS PROVISIONAL — every -DPIN_* below must be reconciled against the
; vendored variants/Seeed_Wio_Tracker_L1/variant.h (see wio-tracker-port.md §2).
extends = nrf52_base
board = seeed_wio_tracker_l1
lib_deps =
    jgromes/RadioLib@^7.0.0
    olikraus/U8g2@^2.35.30
build_flags =
    -DDEVICE_WIO_TRACKER_L1
    -DSIDETONE_BUZZER
    ; --- SX1262 (CONFIRM all against variant.h) ---
    -DPIN_NSS=<n> -DPIN_DIO1=<n> -DPIN_NRST=<n> -DPIN_BUSY=<n>
    -DPIN_SCK=<n> -DPIN_MISO=<n> -DPIN_MOSI=<n>
    ; --- buzzer + buttons (CONFIRM against variant.h) ---
    -DPIN_BUZZER=<n>
    -DPIN_KEY=<n> -DPIN_MODE_BTN=<n>
    ; --- battery (CONFIRM divider/gate against schematic) ---
    -DPIN_VBAT_ADC=<n> -DPIN_VBAT_CTRL=<n>
upload_protocol = nrfutil
```

- BLE comes from the Adafruit core, **not** `lib_deps` (no NimBLE).
- Do **not** add `SX126X_POWER_EN` unless variant.h proves a discrete LoRa-enable
  GPIO (see §2).
- The flash-shrink note from `[env:rak4631]` applies: skip `RADIOLIB_EXCLUDE_*`
  (RadioLib 7.7 partial-exclude breaks the build, and it fits without).

**Acceptance:** `pio run -e wio_tracker_l1` *configures* (resolves the board JSON
+ variant) even before code branches exist — it will fail to *link* until W2–W8,
which is expected.

---

## Phase W2 — `src/pins.h`: `DEVICE_WIO_TRACKER_L1` branch

Add an `#elif defined(DEVICE_WIO_TRACKER_L1)` branch (place it next to the RAK
branch, before the final `#else #error`). Model it on the RAK branch
(`src/pins.h:195`). Consume the `-DPIN_*` flags via `#ifndef` fallbacks that
mirror the env values:

- SX1262: `PIN_NSS/_DIO1/_NRST/_BUSY/_SCK/_MISO/_MOSI` fallbacks (mirror §2).
- **No `HAS_FEM`, no `PIN_FEM_*`** (radio.cpp's FEM blocks compile out).
- **No `SX126X_POWER_EN`** (see §2) — leave it undefined so radio.cpp's
  power-enable block (W3) stays RAK-only.
- OLED: `static constexpr int PIN_OLED_RST = -1;` (display.cpp uses
  `U8X8_PIN_NONE` directly for the Wio, same as RAK).
- Buzzer/sidetone: `#ifndef PIN_BUZZER #define PIN_BUZZER <n> #endif` and
  `static constexpr int PIN_SIDETONE = PIN_BUZZER;` (so `main.cpp`'s
  `sidetone_init(PIN_SIDETONE, TONE_HZ)` passes the buzzer pin).
- Buttons: `static constexpr int PIN_KEY = <n>;` (`Menu_Key`) and
  `static constexpr int PIN_MODE_BTN = <n>;` (`Rot_Key`) — `#ifndef`-guarded if
  set from the env. Comment them with the net names + "CONFIRM against variant.h".
- Battery: `PIN_VBAT_ADC` + `PIN_VBAT_CTRL` fallbacks; document gate polarity
  (a `BATT_GATE_ACTIVE_HIGH`-style note) for W6.

**Acceptance:** `pins.h` compiles standalone for the new define (no missing
symbols); flags in the env are the single source of truth.

---

## Phase W3 — `src/radio.cpp`: share the nRF52 SX1262 path

The RAK path is already exactly what the Wio needs minus the power-enable. Widen
the guards:

- `radio.cpp:8` IRAM_ATTR shim — change
  `#if defined(DEVICE_RAK4631) && !defined(IRAM_ATTR)` →
  `#if (defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)) && !defined(IRAM_ATTR)`.
- `radio.cpp:14` Module-over-global-`SPI` selection — widen
  `#if defined(DEVICE_RAK4631)` → `... || defined(DEVICE_WIO_TRACKER_L1)` so the
  Wio also gets `new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY, SPI)` (not the
  ESP `HSPI` path).
- `radio.cpp:38` TCXO branch — add `|| defined(DEVICE_WIO_TRACKER_L1)` to the RAK
  case (`TCXO_V = 1.8f`). Confirm the Wio-SX1262 module's TCXO voltage from the
  module datasheet/variant; 1.8 V is the safe default.
- `radio.cpp:91` `SX126X_POWER_EN` drive — **keep this RAK-only** (the Wio has no
  such pin; §2). If you widened the guard around `SPI.begin()`, make sure the
  power-enable `pinMode/digitalWrite` stays inside `#if defined(DEVICE_RAK4631)`
  (or `#ifdef SX126X_POWER_EN`).
- **RF switch — RESOLVED in W1 (NOT pure DIO2):** the variant uses `SX126X_DIO2_AS_RF_SWITCH`
  **plus** a discrete `SX126X_RXEN` gate (`LoRa_SW` = D5 / abs GPIO40). Keep
  `setDio2AsRfSwitch(true)` (already present) AND additionally handle RXEN under the
  Wio guard — either register it via RadioLib `chip.setRfSwitchPins(RXEN, RADIOLIB_NC)`
  or drive D5 HIGH for RX (check how Meshtastic's `seeed_wio_tracker_L1` config
  sequences RXEN/DIO2; the vendored `variant.h` + `reference/wio-tracker-l1-pro/README.md`
  document it). **Do not skip RXEN — RX will be deaf without it.**
- **SPI pins:** if the vendored variant's default SPI peripheral pins are *not*
  the LoRa pins, change the Wio `SPI.begin()` to an explicit re-pin. Verify
  against variant.h before assuming the bare `SPI.begin()` is enough.

**Acceptance:** `-e wio_tracker_l1` compiles the radio TU; on hardware the chip
reports its version and `beginFSK()` returns success (W9).

---

## Phase W4 — `src/sidetone_nrf52.cpp`: passive-buzzer backend (NEW)

Add a **third build-time transport** to `sidetone_nrf52.cpp`, selected by
`-DSIDETONE_BUZZER` (set in the Wio env). It is much simpler than the I2S/PAM
DDS paths: a passive piezo just needs a **square wave at the sidetone frequency**,
gated on/off by the key. No DMA, no sine table, no envelope.

Structure (mirror the existing `#if defined(SIDETONE_PAM8403) … #else … #endif`
split — add a `#if defined(SIDETONE_BUZZER)` arm that wins first):

```c
#if defined(SIDETONE_BUZZER)
  // ---- passive piezo buzzer (one GPIO square wave via NRF_PWM0) ----
#elif defined(SIDETONE_PAM8403)
  ...existing...
#else
  ...existing I2S...
#endif
```

Implementation notes for the buzzer arm:

- **Generator:** use `NRF_PWM0` (free in this build — the I2S/PAM arms don't
  compile) to emit a continuous 50%-duty square wave on `PIN_BUZZER`.
  - PRESCALER `DIV_16` → 1 MHz base. `COUNTERTOP = 1_000_000 / freq_hz`
    (e.g. 600 Hz → 1667). One compare value = `COUNTERTOP/2` (50% duty).
  - `DECODER = Common | RefreshCount`, single-entry `SEQ[0]` (`CNT = 1`), and
    free-run with `LOOP = 1` + `SHORTS LOOPSDONE_SEQSTART0` (same restart trick
    the PAM arm uses) so the wave runs with zero CPU once started.
  - PSEL routing: `NRF_PWM0->PSEL.OUT[0] = g_ADigitalPinMap[PIN_BUZZER]` (the
    variant maps Arduino pin == absolute nRF GPIO — same assumption the I2S arm
    relies on); channels 1–3 disconnected (`0x80000000`).
- **`sidetone_init(int gpio, uint32_t freq_hz)`:** `pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);` then configure PWM0 as above but **do not start
  the sequence** (start silent). Store `freq_hz` → COUNTERTOP. No FreeRTOS feeder
  task is needed (nothing to refill).
- **`sidetone_on()`:** if `!s_muted`, `NRF_PWM0->TASKS_SEQSTART[0] = 1;`.
  **`sidetone_off()`:** `NRF_PWM0->TASKS_STOP = 1;` and park the pin LOW. Track
  `s_on` so an unmute while keyed resumes.
- **`sidetone_set_mute(bool)`:** mute → stop + park LOW; remember the gate so
  unmute resumes a held key (match the contract in `sidetone.h`).
- **`sidetone_set_volume(uint8_t)` / `sidetone_set_level(uint8_t)`:** a
  transistor-driven piezo has **little usable amplitude control**. Implement them
  as **coarse or no-ops** — store the value so `show` stays sane, optionally map
  `level` onto PWM duty (lower duty ≈ slightly quieter / thinner), but **document
  that buzzer volume is effectively fixed**. Do not pretend to a smooth curve.
- **Frequency caveat (optional tuning):** the piezo is loudest near its ~2700 Hz
  resonance; a 600 Hz Morse tone will be quiet and buzzy. This is accepted. If
  it's too faint on the bench, consider raising the Wio's default `TONE_HZ`
  toward resonance (a config/`pins.h` default), but keep it operator-configurable.
  Note the trade-off in the file header.

Header comment: explain the three-way transport split and that the buzzer arm is
the Wio Tracker L1 Pro's only option (sealed case, built-in passive piezo, no
amp). Guard the **whole file** stays `#if defined(DEVICE_RAK4631) ||
defined(DEVICE_WIO_TRACKER_L1)` (W8).

**Acceptance:** builds; on hardware a keyed character produces an audible tone
from the internal buzzer; `mute` silences it; unmute mid-key resumes.

---

## Phase W5 — Buttons: key + mode  ✅ (verified — no code needed)

**Resolved with no code change.** Verified against `variants/Seeed_Wio_Tracker_L1/
variant.h`: `PIN_KEY = 13` (D13 = P0.08 = "User/Program Button", the variant's
`CANCEL_BUTTON_PIN`, active-LOW) and `PIN_MODE_BTN = 29` (D29 = `TB_PRESS` = the
5-way joystick centre press). `main.cpp`'s menu/key reads are device-agnostic
(no `#ifdef DEVICE` gate) and use `INPUT_PULLUP` + `LOW == pressed`, which matches
these active-LOW buttons. No pin collisions among the Wio's assigned logical pins
(radio 1/2/3/4/8/9/10, RXEN 5, buzzer 12, key 13, Wire 14/15, vbat 16/30, mode 29).
(Note: there is no `Rot_Key` on this board — only the user button + joystick — so
the joystick press stands in for the mode button.)

Original plan (kept for reference):

Mostly done by the `PIN_KEY` / `PIN_MODE_BTN` defines from W2 — `main.cpp`
already `pinMode(PIN_MODE_BTN, INPUT_PULLUP)` + reads `LOW == pressed`
(`main.cpp:410/419/581`), and `MorseKey::begin(PIN_KEY)` uses `INPUT_PULLUP`,
`LOW == pressed` (`morsekey.cpp`). The Wio buttons are active-LOW with pull-ups,
so **no logic change is needed** — just correct pin numbers.

- Confirm `Menu_Key` (key) and `Rot_Key` (mode) GPIOs from variant.h; set them in
  the env (W1) / `pins.h` (W2).
- If a button shares a pin with something else in the variant, pick a different
  on-board button (the 5-way joystick gives 5 more inputs).
- Optional nicety (not required): debounce window default (`MorseKey` default) is
  fine for a tactile button used as a key.

**Acceptance:** boot-menu navigation works from the mode button (or the 5 s idle
auto-select fires); live-key mode keys the radio + buzzer from the key button.

---

## Phase W6 — `src/battery.cpp`: gated nRF52 analog read

The RAK branch (`battery.cpp:164`) reads an **un-gated** divider. The Wio gates
its divider with `BAT_ADC_CTR`, so it needs the **gated** pattern (like the
Heltec branch) but with nRF52 `analogRead`. Add an
`#elif defined(DEVICE_WIO_TRACKER_L1)` branch:

- `begin()`: `pinMode(PIN_VBAT_CTRL, OUTPUT)`, park at the **disconnect** level;
  `analogReadResolution(12)`.
- `read_volts()`: drive `PIN_VBAT_CTRL` to **connect**, average ~8
  `analogRead(PIN_VBAT_ADC)` samples, park back to disconnect, convert with the
  confirmed `DIVIDER` and the nRF52 ADC reference (the RAK branch shows the
  `analogRead → volts` math; reuse it). **Gate polarity + divider ratio from the
  schematic (§2) — confirm and calibrate.**
- `charging()`: best-effort `false` unless a charge-sense line is trivially
  available (there's a `Sig_Charging` net, but reading it reliably is optional —
  leave `false` for v1).
- Reuse `volts_to_percent()` unchanged.

**Acceptance:** `batt` prints a plausible voltage/% on USB and on battery;
parking the gate avoids idle drain.

---

## Phase W7 — `src/display.cpp`: include the Wio in the U8g2 OLED path

- File guard (`display.cpp:2`) and the closing `#endif` (`display.cpp:232`): add
  `|| defined(DEVICE_WIO_TRACKER_L1)`.
- OLED object: the Wio uses the **same** `U8G2_SSD1306_128X64_NONAME_F_HW_I2C
  oled(U8G2_R0, U8X8_PIN_NONE)` as the RAK — widen the `#if defined(DEVICE_RAK4631)`
  at `display.cpp:22/27` to include the Wio so it shares that constructor (and the
  Heltec VEXT/RST constructor stays Heltec-only).
- `begin()` (`display.cpp:89`): widen the RAK `#if` at `display.cpp:90` to include
  the Wio — **skip VEXT** (no gated rail) and just `Wire.begin()` + `oled.begin()`.
  **I²C pins:** if the variant's default `Wire` pins are not the
  `OLED_I2C_SCL/SDA` pins, call `Wire.setPins(sda, scl)` before `Wire.begin()`
  under the Wio guard. Confirm against variant.h.

**Acceptance:** boot/fox/hunter screens render on the OLED.

---

## Phase W8 — Extend the nRF52 backend guards to include the Wio

The shared nRF52 implementations are currently `#if defined(DEVICE_RAK4631)`.
Widen each to `#if defined(DEVICE_RAK4631) || defined(DEVICE_WIO_TRACKER_L1)`:

- `src/platform_nrf52.cpp` — restart / system_off / reset_reason(+label) /
  unique_id all apply unchanged (nRF52840). **No code change beyond the guard.**
- `src/kv_nrf52.cpp` — LittleFS persistence, identical. Guard only.
  - **VARIANT GAP (found in W3):** the vendored `variants/Seeed_Wio_Tracker_L1/
    variant.h` does **not** define `LED_BUILTIN`, but Adafruit's
    `InternalFileSystem` (`flash_cache.c`) references it → build fails with
    `'LED_BUILTIN' undeclared` the moment kv_nrf52/InternalFS is pulled in. The
    RAK's WisCore variant defines it; this one doesn't. **Fix:** add
    `#define LED_BUILTIN PIN_LED1` to the vendored `variant.h`.
    **TRAP (verified in W5):** the compiler hint says *"did you mean `LED_BLUE`?"*
    — do **NOT** use `LED_BLUE`. In this variant `LED_BLUE = PIN_LED2 = D12 =
    P1.00 = the BUZZER pin` (the variant aliases the buzzer pin as a phantom blue
    LED). Aliasing `LED_BUILTIN` to it would make `flash_cache.c` toggle the
    piezo on every LittleFS write → audible clicking on config saves. Use
    `PIN_LED1` (= 11 = P1.15 = the real Mesh status LED) instead. Record the edit
    in `reference/wio-tracker-l1-pro/README.md` (local change to a vendored file).
    This blocks the W8 link until fixed.
- `src/sidetone_nrf52.cpp` — whole-file guard widened (W4).
- Double-check `platform_esp32.cpp` / `kv_esp32.cpp` guards do **not** accidentally
  include the Wio (they're ESP-only; leave them).

**Acceptance:** `-e wio_tracker_l1` links with no duplicate/missing `platform::`,
`kv::`, or `ble::` symbols; the ESP envs (`heltec_v4`, `heltec_v3`,
`cardputer_adv`) and `rak4631` still build byte-for-byte unchanged.

---

## Phase W9 — Build, fit, flash, verify

**Status: build/fit DONE; FLASHED & BOOTS on real hardware (2026-06-07).**
Remaining: on-air radio, sidetone/buzzer audio, and button checks.

**Flashing gotcha (resolved): SoftDevice version.** The unit ships with **S140
7.3.0** (per `INFO_UF2.TXT`), not the 6.1.1 the board JSON was first pinned to.
Serial `nrfutil` DFU was flaky (timed out / dropped mid-transfer); the reliable
path was the **UF2 mass-storage bootloader** (double-tap reset → `TRACKER L1`
volume → copy `firmware.uf2`). But first the app had to be relinked for the
7.3.0 app base `0x27000` (vendored `nrf52840_s140_v7.ld` + `board_build.ldscript`)
— see `reference/wio-tracker-l1-pro/README.md`. To reproduce:
`pio run -e wio_tracker_l1`, then
`python <fw>/tools/uf2conv/uf2conv.py .pio/build/wio_tracker_l1/firmware.hex -c
-f 0xADA52840 -o firmware.uf2`, then copy to `/Volumes/TRACKER L1/`.

**First boot (confirmed):**
```
# boot #1 reason now=0(POWERON) prev=0(POWERON)
# config: id=115 call=N0CALL wpm=15 ...
# BLE provisioning advertising as "MorseStn-115"
# oled: bus=idle addr=0x3D
# mode=0 station_id=115
# batt 4.18V -> 99%
```
Validated by that one boot: banner + nRF52 reset-reason label, LittleFS config
read (W8), Bluefruit BLE advertising (W8), OLED present on I²C @ 0x3D (W7 probe),
gated battery read ~4.18 V/99% (W6 — `DIVIDER_WIO=2.0` is approximately right).

**Two hardware bugs found on the bench and fixed (both confirmed):**
- **Constant buzzer "ticking":** Bluefruit's auto connection-LED blinks
  `LED_BLUE`, which the variant aliases to `PIN_LED2 = D12 = P1.00 = the buzzer
  pin`. Fixed with `Bluefruit.autoConnLed(false)` in `ble_provision_nrf52.cpp`
  (Wio guard). Commit `93b9bdf`.
- **OLED right-edge distortion:** the panel is an **SH1106** (132-col RAM, 2-col
  visible offset), not an SSD1306 — despite the variant's `USE_SSD1306` flag.
  Fixed by using `U8G2_SH1106_128X64_NONAME_F_HW_I2C` for the Wio in
  `display.cpp` (RAK keeps SSD1306). Commit `4ad5d17`.

**Still to validate on-air:** radio TX/RX + RXEN against a hunter; sidetone tone
quality/keying; button mapping (Menu key + joystick-press menu).

1. **Build:** ✅ `pio run -e wio_tracker_l1` LINKS CLEAN — **186528 B flash
   (22.9% of 815104) / 27748 B RAM (11.2% of 248832)**, fits comfortably. No
   RadioLib-exclude trimming needed.
2. **Regression:** ✅ `heltec_v4`, `heltec_v3`, `cardputer_adv`, `rak4631` all
   still link clean (rak4631 byte-identical at 191100/30492; the others
   unchanged — all Wio changes are behind `DEVICE_WIO_TRACKER_L1` / new #elif
   branches).
3. **Flash:** UF2 / `adafruit-nrfutil` over USB (1200 bps touch), same as RAK.
   The device enumerates as a `usbmodem*` port (covered by `[env]` globs). Use
   `scripts/devices.sh` to map the port.
4. **Validate on hardware (sealed unit — buttons + console + OTA only):**
   - Boot banner + `# boot #N reason=…` line (nRF52 reset-reason labels).
   - `show` / `bootlog` — config + bootlog persist across reflash (LittleFS).
   - BLE-UART console via nRF Connect (Bluefruit NUS) — run a couple of commands.
   - **Radio:** TX/RX against an existing Heltec/RAK hunter on the shared
     frequency; check RSSI/SNR both directions (cf. the RSSI-offset memory note).
   - **Sidetone:** key a character — audible from the built-in buzzer; `mute`
     silences; unmute resumes.
   - **Key/menu:** boot-menu nav from the mode button; live-key from the key
     button.
   - **Battery:** `batt` plausible on USB and on battery.
5. Update `TODO.md` + this file's status board; record the **final** resolved pin
   map in `reference/wio-tracker-l1-pro/README.md`.

**Top unvalidated risks to scrutinize first when a unit is on the bench** (all
built correct-by-construction from the variant/Meshtastic, but untested):
- **Buzzer on→off→on gating (W4):** confirm a re-keyed character re-sounds
  cleanly — `TASKS_STOP` + PSEL-disconnect + GPIO-park, then re-arm via
  PSEL-reconnect + `SEQSTART` on the next `on()`. Watch for a stuck-high pin or
  a click/glitch on re-key. (Also: tone is on `BUZ_PWM`/P1.00 via the correct
  `g_ADigitalPinMap[]` translation — verify the *right* pin sounds.)
- **RXEN RF-switch (W3):** RX must actually hear a hunter. If deaf, the
  `setRfSwitchPins(SX126X_RXEN, SX126X_TXEN)` + `setDio2AsRfSwitch(true)`
  combination may need RXEN driven HIGH manually (comment in radio.cpp notes the
  fallback).
- **Battery divider (W6):** `DIVIDER_WIO = 2.0f` is an upstream placeholder —
  calibrate against a meter; confirm `BAT_CTL` gate polarity (active-HIGH assumed).
- **TCXO voltage (W3):** assumed 1.8 V; if the radio won't `beginFSK()`, check.
- **Buttons (W5):** Menu/User button keys; joystick-press selects the boot menu.

---

## Open questions to resolve on/with hardware (carry into W1–W3)

1. **RF switch:** DIO2-controlled (default, `setDio2AsRfSwitch(true)`) vs a
   discrete `LoRa_SW` GPIO? — from variant.h / Meshtastic config.
2. **LoRa power-enable:** confirm there is **none** (always-on `LoRa_3V3` LDO); if
   variant.h shows one, add it like the RAK `SX126X_POWER_EN`.
3. **TCXO voltage** of the Wio-SX1262 module (assume 1.8 V).
4. **SPI/I²C default pins** in the vendored variant — do they already map to the
   LoRa / OLED pins, or must `radio.cpp` / `display.cpp` re-pin?
5. **Buzzer pin** (`BUZ_PWM`) exact GPIO + that it's plain active-high (it is, per
   the transistor stage).
6. **Battery** divider ratio + `BAT_ADC_CTR` gate polarity.
7. **Buttons** exact GPIOs for `Menu_Key` / `Rot_Key` (and joystick if needed).

Every one of these has a safe default in the plan; the variant.h + a few minutes
on the bench confirm them. None blocks starting W1.
