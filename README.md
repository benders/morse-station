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
