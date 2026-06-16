# `scripts/` — build, flash, and bench-test helpers

Developer tooling for the morse-station firmware: wrappers around PlatformIO,
USB/BLE console drivers, and unattended hardware-in-the-loop test harnesses.

Shell wrappers resolve `pio` via [`_pio.sh`](_pio.sh) (`pio` on `PATH`, else
`~/.platformio/penv/bin/pio`). Python harnesses need `pyserial`; the ones that
also talk BLE need `bleak`. Both live in the project's `tools/blevenv` venv, so
the canonical interpreter is **`tools/blevenv/bin/python`** — the shell wrappers
pick it automatically when present.

## Build / flash / monitor

| Script | Purpose |
|---|---|
| [`build.sh`](build.sh) | Compile (`pio run`); regenerates `compile_commands.json` when stale so clangd stays in sync. Passes extra args through, e.g. `-e heltec_v4`. |
| [`flash.sh`](flash.sh) | Compile + upload over USB. |
| [`run.sh`](run.sh) | Flash, wait for the USB CDC to re-enumerate, then open the monitor. |
| [`monitor.sh`](monitor.sh) / [`monitor.py`](monitor.py) | Serial monitor only (115200). |
| [`compiledb.sh`](compiledb.sh) | Regenerate `compile_commands.json` for clangd (ESP32 toolchain includes). See the clangd note in the memory/AGENTS docs. |
| [`git_rev.py`](git_rev.py) | PlatformIO pre-build hook: injects the current git revision as `-DGIT_REV` (shown by `show` / the boot banner). |
| [`_pio.sh`](_pio.sh) | Internal: resolves the `pio` binary. Sourced by the other shell scripts. |

## Device discovery & provisioning

| Script | Purpose |
|---|---|
| [`devices.sh`](devices.sh) / [`devices.py`](devices.py) | List every connected board with its station ID, mode, mute, and callsign. `--usb` (default) reads the live runtime console **without resetting** (safe to run before flashing a named station); `--boot` resets + adds the board type; `--ble` queries over the air. **Run this first** to map `/dev/cu.*` ports → station IDs (ports re-enumerate between sessions). |
| [`provision.sh`](provision.sh) / [`provision.py`](provision.py) | Provision one unit's NVS (callsign / fox message / station id / mode) over serial. |
| [`provision_all.sh`](provision_all.sh) / [`provision_all.py`](provision_all.py) | Provision **every** connected station at once over the live runtime console. |

## Console drivers (send commands, read replies)

| Script | Purpose |
|---|---|
| [`sercmd.py`](sercmd.py) | Send one or more console commands to a station over USB and print everything it streams back. |
| [`serial_cmd.py`](serial_cmd.py) | Minimal serial-console driver used by the unattended power-management tests. |
| [`ble_cmd.py`](ble_cmd.py) | Send a console command over the always-on **BLE NUS** and print the reply — no reset, no USB. E.g. `tools/blevenv/bin/python scripts/ble_cmd.py 73 "show"`. |
| [`serial_capture.py`](serial_capture.py) | Resilient capture that survives USB unplug/replug (used for the Cardputer brownout test). |
| [`station_serial.py`](station_serial.py) | Shared USB-serial helpers imported by the other Python harnesses. |

## Integration & bench tests

These drive real hardware end-to-end and assert on passively-observed serial
output (most require `debug on`). Run after mapping ports with `devices.sh`.

| Script | What it validates |
|---|---|
| [`broadcast_test.py`](broadcast_test.py) | **Instructor broadcast banner** (`bcast`). Four stations (Instructor / Fox / two Hunters): the instructor relays a `msg`, then `stop`, then a `bcast` banner, and both hunters confirm reception via the passive `RX B … text=…` line plus a `show` "banner =" cross-check. See `docs/plan-instructor-broadcast.md`. |
| [`broadcast_display_test.py`](broadcast_display_test.py) | Broadcast banner **display** check: halts the Fox, force-blanks both Hunter panels, then broadcasts and asserts each Hunter **wakes** (`# screen: woke`) and holds the banner lit (no re-blank) — i.e. the banner overrides idle-blank and stays up for its minute. |
| [`relay_stopstart_test.py`](relay_stopstart_test.py) | **Instructor remote control** stop/start: the instructor relays `stop`, hunters finish + screen-blank, then a new `msg` + `start` wakes them and they copy the new message. Also checks BLE responsiveness after auto-wake. |
| [`rc_test.py`](rc_test.py) | Instructor remote-control end-to-end: concurrently watches fox + instructor + hunters during a relay run. |
| [`edge_test.py`](edge_test.py) / [`edge_test.sh`](edge_test.sh) | Edge-event keying decode harness (`docs/edge-events.md`, E6). |
| [`foxhunt_roundrobin.py`](foxhunt_roundrobin.py) | Round-robin fox-hunt link test over USB (edge protocol). |
| [`watchdog_test.py`](watchdog_test.py) | Hardware-watchdog validation: clear the bootlog, wedge the node (`stall`), confirm the watchdog reboots it. |
| [`tx_bench.py`](tx_bench.py) | TX-power bench comparison across stations. |
| [`lna_test.py`](lna_test.py) | A/B the V4.3 RX LNA (CSD select) on a hunter while a fox transmits. |
| [`sdr_drift.py`](sdr_drift.py) | Per-device carrier frequency-drift measurement via an `rtl_tcp` SDR (drives `txcw`). |

### Serial gotchas

- **Ports re-enumerate** between plug-ins — never hard-code `/dev/cu.*`; map with
  `devices.sh --usb` each session.
- **ESP32 native USB** (Cardputer, Heltec V4) and **CP2102** (Heltec V3) **reset
  on port open** — allow ~6–8 s for boot before sending commands.
- **Wio Tracker L1 (nRF52)** does **not** reset on open and needs **DTR asserted**
  to accept input; its TinyUSB CDC also **drops the port across reboots**
  (`Device not configured`). Harnesses that reconfigure it must auto-reconnect on
  I/O failure and allow ~10 s settle after a reboot before reopening — see
  `broadcast_test.py`'s `Station` class for the pattern.
