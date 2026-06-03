# Serial / BLE Command Reference

Every station exposes one **line-oriented command console** for provisioning and
live control: setting the callsign, fox message, station ID, keying speed, TX
power, boot mode, and mute. The same grammar is reachable over three transports
(below), all dispatched by one parser — `handle_setup_command()` in
`src/main.cpp`. There is no prompt or banner on the runtime/BLE paths: send a
command line terminated by CR or LF and read the echoed result.

## Commands

| Command | Effect | Persisted? | Live-applies? |
|---|---|---|---|
| `call <SIGN>` | Set operator callsign (≤ 10 chars). Rides in the Ident packet and should also appear in the fox message text for an audible CW ID. | yes (NVS) | yes — read live by the TX path |
| `msg <text...>` | Set the canned fox message (≤ 96 chars). | yes | yes — used at the next message loop |
| `id <n>` | Set station ID, 1..254 (255 = broadcast). Overrides the eFuse-MAC default. | yes | yes |
| `wpm <n>` | Overall (effective) keying speed, clamped **5..40**. Default 15. Raising it above `char_wpm` also raises `char_wpm` to match. | yes | fox only: re-arms the player at the next message loop |
| `farns <n>` | Farnsworth **character** speed, clamped **wpm..40**. Default 18. Equal to `wpm` ⇒ plain timing. | yes | fox only: re-arms the player at the next message loop |
| `pwr <0..3>` | Fox TX power: `0`=LO(−9) `1`=MED(+2) `2`=HI(+14) `3`=MAX(+22 dBm chip). Out-of-range prints the legend. | yes | fox only: retunes the SX1262 immediately |
| `mode <0..2>` | Set the **boot** mode: `0`=Hunter `1`=Fox `2`=Livekey. (Hibernate is not selectable here.) Takes effect on next boot — pair with `reboot`. | yes | no (boot-time) |
| `mute [on\|off]` | Sidetone mute. Bare `mute` toggles; `on`/`1` and `off`/`0` set explicitly. For a node running near people. | yes | yes — applied immediately |
| `show` | Print the current config: `id`, `call`, `wpm`, `farns`, `mute`, boot `mode`, `msg`. | — | — |
| `bootlog` | Dump the boot/crash-reason ring (last 16 boots, oldest first): `#<boot> reason=<n>(<name>)`. Diagnoses crashes after the fact with no serial attached at reset time. `bootlog clear` empties the ring (the monotonic boot counter is kept). | — | reads NVS |
| `reboot` / `restart` | Soft-reset the node. The boot menu auto-selects the stored boot mode after its idle timeout, so this applies a `mode <n>` change with **no physical interaction**. | — | — |
| `done` / `exit` | End the (blocking boot) console session and continue to the run mode. No-op at runtime / over BLE. | — | — |
| anything else | Prints the one-line usage legend. | — | — |

**Live-apply** only happens once `setup()` has brought up the radio and run mode
(the `g_live_apply` flag). During the **boot** serial/keyboard console the parser
only writes NVS; values take effect at the normal boot points. This keeps the
bench path behaviorally identical regardless of transport.

## Transports

### 1. Boot serial console (bench)

A blocking REPL offered briefly at power-on over USB-CDC (115200 baud).

- On boot the firmware prints the current config and `# send 's' within 1s for
  setup console...`. Send `s` (or `S`) within ~1 s to enter the REPL; otherwise
  it prints `(skipped)` and continues to the boot menu.
- Inside, it prompts `setup> ` and loops until `done`/`exit`.
- DTR/RTS auto-reset is best-effort. If the 1 s window is missed, tap RST (or
  pass `--port` to the host helper, below) and retry.

### 2. Runtime serial console (development)

The non-blocking sibling of the BLE path. After a mode has started, any complete
command line arriving on Serial is fed to the same parser, so a USB terminal can
drive a **running** station (`show` / `mute` / `mode` / `pwr` / `wpm` …) with
close parity to the over-the-air BLE console. No prompt; type a command + Enter.
The `done`/`exit` return is ignored (there is no session to end at runtime).

### 3. BLE — Nordic UART Service (field, preferred)

NimBLE stands up a **Nordic UART Service (NUS)** peripheral that stays up for the
whole session, so an operator can re-provision a *running* node over the air — no
laptop, no USB, no physical access. Any generic BLE-UART terminal app works
(nRF Connect, LightBlue, Bluefruit Connect); **no custom app required**. This was
chosen because iOS exposes only BLE GATT (not classic SPP) to third-party apps.

- **Advertised name:** `MorseStn-<station_id>` (per-unit, so units are
  distinguishable from a Mac where CoreBluetooth hides the MAC).
- **Service UUID:** `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - **RX** (phone → device, write): `6E400002-…` — send command lines here,
    CR/LF-terminated.
  - **TX** (device → phone, notify): `6E400003-…` — responses, chunked to a safe
    20-byte MTU.
- **Always-on & reconnectable:** advertising restarts on disconnect, so you can
  connect, tweak, disconnect, and reconnect throughout an exercise.
- **Thread safety:** the RX `onWrite` callback runs on the NimBLE host task and
  only *queues* completed lines (FreeRTOS queue, depth 4). `ble_provision::process()`
  drains and dispatches them from `loop()` — the same task that reads/writes NVS
  config — so the parser never runs concurrently with config access. If the main
  loop is behind, lines are dropped rather than blocking the host task
  (provisioning is interactive and low-rate).
- **Coexistence:** the SX1262 is a separate SPI sub-GHz radio, so BLE on the
  ESP32-S3 2.4 GHz core coexists with fox TX at no RF cost — only heap (~10 KB
  RAM) and a little CPU. Verified on hardware with fox TX (30 ms keystate) and
  BLE connected. Flash budget: Heltec V4 ~18 %, Cardputer ~23 %.

#### ⚠ No authentication

The NUS is **open** — anyone in BLE range with a generic BLE-UART app can
reconfigure or reboot a node. For a hobby fox hunt this is an acceptable trade
for zero-friction access. If it ever matters, add NimBLE bonding / a passkey, or
gate writes behind a shared secret.

## Examples

Provision a unit as the fox `KC8HOB` at 15/18 WPM, then send it on-air over the
air:

```
call KC8HOB
msg DE KC8HOB FOX NEAR THE BIG OAK BY THE LAKE
id 42
wpm 15
farns 18
mode 1
show
reboot
```

Silence a node that's near people, live:

```
mute on
```

## Host helpers

`scripts/` contains host-side helpers (see each script):

- `scripts/provision.sh` / `provision.py` — push `call`/`msg`/`id`/… over serial.
- `scripts/devices.py` — map USB ports ↔ station IDs / mode / mute (`--usb`
  reads a running node without resetting it; `--ble` over the air).
- `scripts/monitor.sh` / `monitor.py` — serial monitor.
- `tools/ble_provision_test.py` — bleak-based round-trip test of the BLE NUS.
