# Morse Station

Target board env: `heltec_v4` (set via `-e` if needed).

```sh
scripts/build.sh                 # compile
scripts/flash.sh                 # compile + upload over USB
scripts/run.sh                   # flash, then open serial monitor
scripts/monitor.sh               # serial monitor only (115200 baud)
```

Pass extra PlatformIO args through, e.g. select an env:

```sh
scripts/build.sh -e heltec_v4
scripts/flash.sh -e heltec_v4
```

Requires PlatformIO (`pio` on `PATH`, or installed at
`~/.platformio/penv/bin/pio`).

## Operating

On boot the OLED shows a mode menu. Use the **PRG/BOOT** button:

- **Short press** — cycle the highlighted mode.
- **Long press (>= 0.6 s)** — select it. (Auto-selects after 8 s idle.)

Modes:

- **Hunter (RX)** — receives, plays the Morse sidetone, and shows a scrolling
  line of the last decoded characters, the transmitting fox's **station ID and
  frequency** (`RECV <id>`, from the received packets), and an RSSI bar (fuller bar
  = stronger signal, a warmer/colder hint). The sidetone **volume tracks signal
  strength** (dB-linear, so it sounds like a classic "tune for max volume"
  meter) — run the fox at LO power so the signal actually falls off with distance
  and the gradient is audible as you close in. A **short PRG tap cycles the
  sidetone volume** MUTE / LOW / MED / HIGH (remembered across power cycles;
  buzzer/PAM8403 boards toggle mute only). The copy line shows raw dit/dah
  elements by default; switch it to decoded letters with the `showtext on`
  console command (see `docs/commands.md`).
- **Fox (TX loop)** — repeats the canned location message on the air. A **short
  PRG tap cycles TX power** LO / MED / HI / MAX (shown as "PWR x" on the OLED).
  The level is **remembered across power cycles** (boots LO the first time) —
  pull it down to LO in a small space. MAX is the SX1262 chip ceiling (+22 dBm),
  which still exceeds the Part-15 §15.249 limit, so only use it under an amateur
  license. (On the Heltec V4 a FEM PA could add more, but its PA mode is left
  unmanaged today — see the [issues](https://github.com/benders/morse-station/issues);
  the Cardputer has no FEM, so +22 dBm is its real antenna ceiling.)
- **Live key (TX)** — transmits a telegraph key wired to the key GPIO, with
  local sidetone, so students can key to a hunter.
- **Instructor (RC)** — remote control over the GFSK radio (not just BLE range).
  Lets an exercise leader re-tune a *distant* fox live — new clue text, speed,
  power — by relaying the normal console commands on-air to a target station,
  which applies them and acks back. Drive it from the instructor's own
  BLE/serial console: `relay <id> <cmd>` (or `relay 255 <cmd>` to reach every
  station). `alert <text>` pushes a plaintext banner to **every** station's
  screen and sounds a short attention tone there — **even on muted units** — so
  operators notice; `alert clear` dismisses it. See `docs/commands.md` →
  "Instructor (remote control over GFSK)".
- **Hibernate** — power-off stand-in (the boards have no hardware switch):
  powers down the front-end and peripheral rail and enters deep sleep. Press
  **RST** to wake — the sketch restarts back into this menu. So RST turns the
  unit off (via Hibernate) and on again in the field.

The menu starts highlighted on the **last-used mode** (remembered across power
cycles). To re-pick a mode, reset the board (the menu runs again at boot).

## Documentation

- [`docs/protocol.md`](docs/protocol.md) — over-the-air protocol, link
  parameters, packet formats, addressing, and the regulatory basis (§15.249 /
  Part 97, FHSS plan).
- [`docs/commands.md`](docs/commands.md) — the serial / BLE command reference
  (provisioning and live control over all three transports).
- [`docs/components/`](docs/components/) — per-component lessons learned:
  [Heltec V4](docs/components/heltec-v4.md),
  [Heltec V3](docs/components/heltec-v3.md),
  [Cardputer ADV](docs/components/cardputer-adv.md),
  [SX1262](docs/components/sx1262.md),
  [MAX98357A](docs/components/max98357a.md).
- [`docs/debug-heltec-v4.md`](docs/debug-heltec-v4.md) — capturing panic
  backtraces over UART/JTAG.
- [`scripts/README.md`](scripts/README.md) — build/flash wrappers, device
  discovery (`devices.sh`), console drivers, and the hardware-in-the-loop test
  harnesses (incl. the instructor relay / broadcast tests), with the per-board
  serial gotchas.
- [GitHub Issues](https://github.com/benders/morse-station/issues) — open work · [`DONE.md`](DONE.md) — completed work.
