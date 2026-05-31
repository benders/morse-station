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
  line of the last decoded characters, the transmitting fox's **callsign and
  frequency** (from its periodic station-ID packet), and an RSSI bar (fuller bar
  = stronger signal, a warmer/colder hint). The sidetone **volume tracks signal
  strength** (dB-linear, so it sounds like a classic "tune for max volume"
  meter) — run the fox at LO power so the signal actually falls off with distance
  and the gradient is audible as you close in. A **short PRG tap toggles the copy
  line** between decoded letters (default) and raw dit/dah elements (a learning
  aid).
- **Fox (TX loop)** — repeats the canned location message on the air. A **short
  PRG tap cycles TX power** LO / MED / HI / MAX (shown as "PWR x" on the OLED).
  The level is **remembered across power cycles** (boots LO the first time) —
  pull it down to LO in a small space. MAX (~28 dBm) exceeds the Part-15 §15.249
  limit, so only use it under an amateur license.
- **Live key (TX)** — transmits a telegraph key wired to the key GPIO, with
  local sidetone, so students can key to a hunter.
- **Hibernate** — power-off stand-in (the boards have no hardware switch):
  powers down the front-end and peripheral rail and enters deep sleep. Press
  **RST** to wake — the sketch restarts back into this menu. So RST turns the
  unit off (via Hibernate) and on again in the field.

The menu starts highlighted on the **last-used mode** (remembered across power
cycles). To re-pick a mode, reset the board (the menu runs again at boot).
