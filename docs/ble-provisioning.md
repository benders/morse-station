# BLE Field Provisioning — Design Plan

Goal: let a phone (or a "master" unit) set callsign / fox-message / station-id /
wpm / farns / boot-mode over **BLE** in the field, without a laptop + USB. The
serial console stays as the bench path. The ESP32-S3 has BLE on-silicon, so no
extra hardware is needed.

This doc captures the plan. **Steps 0–2 are done**: the parser refactor, the
NimBLE NUS transport, and always-on field provisioning of a running node.

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

## Step 1 — BLE-UART transport (DONE)

- Added **NimBLE-Arduino** to the Heltec + Cardputer envs (`lib_deps`).
- `src/ble_provision.{h,cpp}` stands up a NUS peripheral: advertises as
  `MorseStn-<station_id>`, RX characteristic (write) + TX characteristic (notify).
- `BleOut : public Print` wraps the TX characteristic, chunking `notify()` to a
  safe 20-byte MTU, so every response line in `handle_setup_command` flows back
  unchanged.
- The RX `onWrite` callback assembles bytes into a CR/LF-terminated line.

## Step 2 — Always-on field provisioning (DONE)

We took the **always-on** path: NimBLE stays up for the whole session (not just a
boot window), so an exercise operator can adjust a *running* node over the air —
no reset, no laptop, no physical access.

- **Coexistence:** the SX1262 is a separate SPI sub-GHz radio, so it coexists with
  the ESP32-S3 2.4 GHz BLE core; the cost is heap + a little CPU, not RF. Verified
  on hardware running fox TX (30 ms keystate) with BLE connected.
- **Thread safety:** the RX `onWrite` runs on the NimBLE host task, but
  `config::set_*` mutates the RAM-cached config and writes NVS. To keep that
  single-threaded, `onWrite` only *queues* completed lines (FreeRTOS queue);
  `ble_provision::process()` drains and dispatches them from `loop()` — the same
  task that reads config. Responses notify back from that task.
- **Live apply (params + TX power):** a `g_live_apply` flag (set once setup() has
  brought up the radio + run mode) lets the parser apply changes to the running
  node. In fox mode, `wpm`/`farns` re-arm the player (effective at the next
  message loop) and a new `pwr <0..3>` command retunes the SX1262 immediately.
  `call` / `id` / `msg` are read live by the TX paths already. Before
  `g_live_apply`, the boot serial/keyboard console behaves exactly as before
  (NVS-only, applied at the normal boot points).
- **Mode changes via remote reboot:** switching operating mode (Hunter/Fox/Livekey)
  live is out of scope (it would mean tearing down and re-initing the radio and
  per-mode state). Instead, `mode <n>` sets the boot mode and a new `reboot`
  command resets the node; the boot menu auto-selects the stored mode after its
  idle timeout, so the node comes back in the new mode with **no physical
  interaction**.
- **Reconnectable:** server callbacks re-start advertising on disconnect, so the
  operator can connect, tweak, disconnect, and reconnect throughout the exercise.

Grammar (serial and BLE share it):
```
call <SIGN> | msg <text> | id <n> | wpm <n> | farns <n> | pwr <0..3> |
mode <0..2> | show | reboot | done
```

## Caveats / watch-items

- **No authentication:** the NUS is open — anyone in BLE range with a generic
  BLE-UART app can reconfigure or reboot a node. For a hobby fox hunt that is an
  acceptable trade for zero-friction access; if it ever matters, add NimBLE
  bonding / a passkey, or gate writes behind a shared secret.
- **Flash/heap budget:** NimBLE + RadioLib is heavy but fits — Heltec V4 ~18 %
  flash, Cardputer ~23 %; always-on NimBLE adds ~10 KB RAM at runtime.
- **ESP-NOW fallback:** an ESP-NOW broadcast from a "master" unit remains a lighter
  alternative for one-to-many push (see `esp-now.md`).
