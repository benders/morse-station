# Debugging the Heltec V4 (capturing panics / backtraces)

The Heltec WiFi LoRa 32 V4 is an **ESP32-S3** talking to the host over its
**native USB-C** (built-in USB-Serial-JTAG; we build with
`ARDUINO_USB_MODE=1` = "Hardware CDC and JTAG", `ARDUINO_USB_CDC_ON_BOOT=1`).
That USB CDC is convenient but a poor debugger:

- **Early boot prints are lost** — the host re-enumerates on every reset, so the
  bootloader banner and our `# morse-station build …` / `# mode=…` lines are
  usually gone before a terminal re-attaches.
- **A boot loop drops the port repeatedly**, which also stops `esptool` from
  auto-entering download mode (you must hold **PRG** + tap **RST** to flash).
- **Crucially, the panic backtrace does NOT go to USB.** The ESP-IDF panic
  handler and ROM log write to **UART0**, which on this board is a separate set
  of pins with nothing attached — so a crash looks like total silence on USB.

Lesson learned the hard way: the `sidetone_init()` I2S-PDM bring-up
(`-DSIDETONE_PDM`) hangs/panics at boot — blank OLED, zero USB serial — and we
had no backtrace because we were only listening on USB. See
`platformio.ini` (parked behind the flag) and `src/sidetone.cpp`.

## Free debug pins (this firmware)

Nothing in `src/pins.h` uses GPIO39–44, so both debug interfaces are available:

| Function | GPIO | Notes |
|---|---|---|
| UART0 TX (`U0TXD`) | **43** | panic backtrace + bootloader log come out here |
| UART0 RX (`U0RXD`) | **44** | only needed if you want to type to it |
| JTAG MTCK | 39 | for an external ESP-Prog (not needed — see below) |
| JTAG MTDO | 40 | |
| JTAG MTDI | 41 | |
| JTAG MTMS | 42 | |

GPIO43/44 are broken out on the Heltec V3/V4 castellated header (labeled Tx/Rx).

## Option A — External USB-UART on UART0 (most reliable for a panic)

The backtrace is already being printed on **GPIO43**; you just need to listen on
that wire with something that does not drop on reset.

1. Wire a $3 CP2102/CH340 USB-UART adapter:
   - adapter **RX → GPIO43** (board TX)
   - adapter **GND → GND**
   - (adapter TX → GPIO44 only if you need to send to the board)
2. Open it at **115200 8N1** — e.g. `~/.platformio/penv/bin/pio device monitor
   --port /dev/cu.usbserial-XXXX --baud 115200` (note: a real `tty`, so run it in
   an interactive terminal, not a captured/background shell).
3. Reset the board (tap **RST**). You now get the full ROM/bootloader log and,
   on a crash, the `Guru Meditation …` panic with the backtrace.

To decode a `Backtrace: 0x… 0x…` line into source:

```sh
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line \
  -pfiaC -e .pio/build/heltec_v4/firmware.elf  0x... 0x... 0x...
```

## Option B — JTAG over the existing USB-C (no extra hardware)

Because we build in "Hardware CDC and JTAG" mode, the S3's built-in
USB-Serial-JTAG exposes a **JTAG interface on the same USB-C cable**. Run
OpenOCD + gdb with zero added hardware — good for stepping through
`i2s_driver_install` to see exactly which assert fires, or halting on the abort.

```sh
# Terminal 1: OpenOCD against the built-in USB-Serial-JTAG
openocd -f board/esp32s3-builtin.cfg

# Terminal 2: gdb
~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb \
  .pio/build/heltec_v4/firmware.elf \
  -ex 'target remote :3333' -ex 'mon reset halt'
# then: break abort / continue / bt
```

OpenOCD can halt the core under reset, which sidesteps the boot loop better than
serial can. Caveat: the USB-Serial-JTAG CDC still re-enumerates on reset, so for
the *crash backtrace specifically* the external UART (Option A) is the most
dependable capture.

## Identifying which board is which on USB

Port names (`/dev/cu.usbmodem…`) drift between resets, and the Cardputer and
Heltecs can swap. Confirm by the battery-log format on the serial console:

- **Heltec V4** → `# batt 4.09V -> 94%` (voltage + percent)
- **Cardputer** → `# batt 100%` (percent only)

A Heltec stuck before `loop()` prints **nothing** (a healthy one logs `# batt …`
every 2 s).
