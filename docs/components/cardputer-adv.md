# Component notes — M5Stack Cardputer ADV

M5Stack Cardputer ADV (Stamp-S3A / ESP32-S3) used as an **experimental fox**:
onboard speaker, QWERTY keyboard, colour LCD, and a removable LoRa radio. Built
with `-DDEVICE_CARDPUTER_ADV`; shares `src/` with the Heltec, with device code
behind that define or in `*_cardputer.cpp` files. The radio is covered in
`sx1262.md`.

## Hardware overview

- **MCU:** Stamp-S3A (ESP32-S3), **8 MB flash** (`board_build.flash_size = 8MB`,
  `default_8MB.csv` partitions).
- **Radio: NOT onboard.** It rides on the removable **Cap LoRa-1262** — a bare
  **SX1262** + ATGM336H GNSS + RP-SMA antenna, **no external PA/FEM**.
- **Display:** ST7789V2, 240×135 colour.
- **Audio:** ES8311 codec + NS4150B amp over I2S — no PWM-to-pin shortcut.
- **Keyboard:** TCA8418 I2C keypad controller.
- **Power:** 1750 mAh pack, reported via M5Unified PMIC.

## Pin map (`src/pins.h`)

| Function | GPIO | Notes |
|---|---|---|
| SX1262 NSS / SCK / MOSI / MISO | 5 / 40 / 14 / 39 | SPI **shared with the microSD slot** |
| SX1262 RST / BUSY / DIO1 | 3 / 6 / 4 | control lines from the freed keyboard-scan block |
| LCD BL / RST / DC | 38 / 33 / 34 | BL shared with RGB-LED power-enable |
| LCD MOSI / SCK / CS | 35 / 36 / 37 | |
| Internal I2C SDA / SCL | 8 / 9 | shared bus |
| Keyboard INT | 11 | TCA8418, active-low / falling edge |
| TCA8418 / ES8311 / BMI270 | 0x34 / 0x18 / 0x68 | I2C addresses on the shared bus |
| ES8311 I2S SCLK/ASDOUT/LRCK/DSDIN | 41 / 46 / 43 / 42 | no MCLK; M5.Speaker owns these |
| Mode button | 0 | onboard G0 (BtnA / BOOT) |

## Lessons learned

### M5Unified owns the peripherals — go through it

`M5.begin()` claims the LCD, ES8311 codec, and PMIC. So:

- **Display** uses `M5.Display` (M5GFX, same LovyanGFX family) rather than a
  standalone instance; standalone LovyanGFX was dropped from `lib_deps` (M5GFX
  ships with M5Unified). All bring-up funnels through `cardputer_m5_begin()`
  (`platform_cardputer.cpp`), which is idempotent.
- **Sidetone** is `M5.Speaker.tone()` / `.stop()` (`sidetone_cardputer.cpp`),
  same `sidetone_{on,off,set_volume,set_mute}` API as the Heltec LEDC path.
  M5.Speaker runs the I2S/codec on its own RTOS task, so key on/off carries no
  latency. **Software volume exists here** (`M5.Speaker.setVolume`,
  0..255) — unlike the Heltec, which has only a hardware pot.
- **Battery** is `M5.Power.getBatteryLevel()` / `isCharging()` (`battery.cpp`).

### Internal I2C must be re-begun after `M5.begin()` (dead-keyboard fix)

The TCA8418 keyboard (0x34) shares the internal I2C bus (GPIO8/9) with the codec
and IMU. After `M5.begin()` the keyboard would NACK (reads return 0x00, keypad
dead) until `M5.In_I2C` is **re-begun** following `M5.begin()`. This is **not** a
StampS3 misdetect, not a BLE issue, and not a battery problem — the board detects
fine. (See the saved memory note.) The keyboard driver reads the TCA8418 over
`M5.In_I2C` (no second I2C master) and polls the event FIFO.

### Display flicker → full-frame canvas

Clearing + redrawing the panel directly each ~10 Hz frame flickered. Fixed by
rendering into a full-frame `M5Canvas` back buffer and `pushSprite`-ing it once
per frame. Stable on HW (ran ~100 s, heap flat at 275 KB, zero reboots).

### TCXO is 1.8 V (confirmed)

The Cap LoRa-1262's SX1262 uses a 1.8 V TCXO, not a plain crystal — `beginFSK()`
succeeds and the cap decoded real off-air Morse as a hunter. (If a different cap
fails init, suspect this first — see `sx1262.md`.)

### No FEM → +22 dBm is the real ceiling

With no external PA, the SX1262 chip output **is** the antenna power: MAX (+22
dBm) is the true ceiling, no FEM bring-up needed (simpler than the Heltec).

### Open issues

- **Poor RX sensitivity as a hunter.** With devices ~1 ft apart the RSSI gauge
  sits ~halfway, where a Heltec pins near full. Some gap is expected (bare SX1262
  with no LNA vs the Heltec's FEM LNA), but halfway at a foot is worse than that.
  To diagnose: confirm the RP-SMA antenna is seated, compare raw RSSI cap vs
  Heltec at a fixed distance, recheck RX config (`rxBw`, `setRxBoostedGainMode`),
  and check the shared SPI/SD bus isn't degrading the link. May make the cap
  fox-only. See `TODO.md`.
- **Intermittent crash while typing on the keyboard** (seen once, not yet
  reproduced, no panic capture). Suspects: TCA8418 FIFO/keynum decode, or the
  editor buffer bounds. The `# boot #N reason` NVS log records the reset reason
  for the next recurrence.

## Other notes

- **Live-key mode** needs a physical key on a free GPIO (Grove G1/G2 — confirm);
  out of scope for the experimental fox. `PIN_KEY` is parked at GPIO1 for parity.
- The cap's **ATGM336H GNSS** could later auto-populate the fox's grid/coords in
  the message.
