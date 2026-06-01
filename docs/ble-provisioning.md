# BLE Field Provisioning — Design Plan

Goal: let a phone (or a "master" unit) set callsign / fox-message / station-id /
wpm / farns / boot-mode over **BLE** in the field, without a laptop + USB. The
serial console stays as the bench path. The ESP32-S3 has BLE on-silicon, so no
extra hardware is needed.

This doc captures the plan. **Step 0 (the parser refactor) is done**; the BLE
transport itself is not yet built.

## Why this is cheap on the firmware side

The existing setup console (`run_setup_console` in `src/main.cpp`) is already a
plain line REPL with a tiny text grammar:

```
call <SIGN> | msg <text> | id <n> | wpm <n> | farns <n> | mode <0..2> | show | done
```

That is exactly the kind of text a generic BLE-UART terminal app (nRF Connect,
LightBlue, Bluefruit Connect) sends. So a BLE config session is a drop-in for the
serial session — the only thing tying the grammar to the UART was that input came
from `Serial` and output went to `Serial.printf`.

## iOS / generic-app constraint

iOS only exposes **BLE GATT** to third-party apps (not classic Bluetooth SPP). So
the firmware must advertise as a **BLE peripheral with a writable
characteristic**. If we expose the **Nordic UART Service (NUS)** — a well-known
RX (phone→device write) + TX (device→phone notify) pair — then off-the-shelf apps
give a real serial-style terminal with **no custom phone app required**. A custom
SwiftUI/CoreBluetooth app is a later ergonomics-only nicety (buttons, presets,
RSSI/battery readback).

## Step 0 — Parser refactor (DONE)

Decouple the command *dispatch* from the *I/O transport* so serial and BLE can
share one parser:

- `handle_setup_command(const char* line, Print& out)` — pure dispatch: takes one
  command line, mutates NVS config, writes responses to a `Print&` sink. Returns
  `true` on `done`/`exit` so a caller can end its session.
- `run_setup_console()` now just reads a line from `Serial` and calls
  `handle_setup_command(line, Serial)`. `Serial` already *is* a `Print`, so this
  is a behavioral no-op on the bench path.

This unblocks BLE without committing to the NimBLE dependency yet, and it makes
the dispatch independently testable.

## Step 1 — BLE-UART transport (TODO)

- Add **NimBLE-Arduino** to the Heltec + Cardputer envs (`lib_deps`).
- Stand up a NUS peripheral: advertise, RX characteristic with an `onWrite`
  callback, TX characteristic for notify.
- Wrap the TX characteristic in a small `Print` subclass whose `write()` calls
  `pTxChar->notify()` — call it `BleOut`. Then every existing response line in
  `handle_setup_command` flows back to the phone unchanged.
- In the RX `onWrite` callback, buffer bytes into a line (strip CR/LF, same as
  `serial_read_line`) and call `handle_setup_command(line, bleOut)`.

## Step 2 — When BLE is live (TODO / decisions)

- **Boot-window config (smallest, preferred first):** start advertising in the
  same opt-in window as the serial console; config takes effect on reboot, exactly
  like the serial path today. Mirrors existing semantics.
- **Always-on control (bigger lift):** keep NimBLE up in the main loop to change
  wpm / power / mode live. Requires making those config changes take effect at
  runtime, not just on reboot.

## Caveats / watch-items

- **Flash budget:** NimBLE + RadioLib is heavy. The Heltec V4 is the smaller part;
  `pio run` size-check before committing. The Cardputer (8 MB) has more room.
- **Radio coexistence:** BLE and the SX1262 both lean on the ESP32 radio-core
  scheduler. The fox transmits keystate every 30 ms; prefer BLE config as a
  **boot-time / idle-time** activity, not concurrent with active fox TX, to avoid
  timing jitter on the keystate stream.
- **ESP-NOW fallback:** if BLE proves fiddly, an ESP-NOW broadcast from a "master"
  unit is a lighter alternative (see `esp-now.md`).
