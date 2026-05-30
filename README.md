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

- **Hunter (RX)** — receives, plays the Morse sidetone, shows decoded text and
  an RSSI bar (fuller bar = stronger signal, a warmer/colder hint).
- **Fox (TX loop)** — repeats the canned location message on the air.
- **Live key (TX)** — transmits a telegraph key wired to the key GPIO, with
  local sidetone, so students can key to a hunter.

To re-pick a mode, reset the board (the menu runs again at boot).
