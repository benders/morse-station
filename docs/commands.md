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
| `ipwr <0..3>` | **Instructor mode only.** Runtime TX power override (same level scale as `pwr`). Instructor always boots at MAX; use this to drop power on the bench. **Not persisted** — next boot resets to MAX. | no (RAM) | yes — retunes SX1262 immediately |
| `mode <0..3>` | Set the **boot** mode: `0`=Hunter `1`=Fox `2`=Livekey `3`=Instructor. (Hibernate is not selectable here.) Takes effect on next boot — pair with `reboot`. | yes | no (boot-time) |
| `relay <id\|255> <cmd...>` | **Instructor mode only.** Send any command above to a *distant* station over the GFSK radio (not just short-range BLE). `id` is the target station, or `255` to broadcast to all. The target runs `<cmd>` through this same parser and returns an ack, shown on the instructor's screen/serial as `ACK <id>: <reply>`. Alias: `fox <id> <cmd>`. See "Instructor (remote control)" below. | n/a (the *target's* command persists per its own rule) | n/a |
| `alert <text...>` | **Instructor mode only.** Push a short plaintext **banner** (≤ 40 chars) to the screen of *every* station — Fox, Hunter, Instructor — for **1 minute**, regardless of mode, **and sound a ~1.5 s attention tone there**. Unlike `relay`, nothing is executed: the literal text is shown on the panel (centered, wrapped to two lines, or horizontally scrolled if too long). The banner **always force-wakes a blanked panel** and keeps it lit for its full minute; the tone sounds **even on a muted node** (it overrides `mute` for its duration without changing the persisted mute state). `alert clear` dismisses it everywhere (and is silent). Fire-and-forget (no ack): the same message is bursted 5× over ~7.5 s for delivery. A button tap / keypress on any station dismisses its banner. See "Instructor alert" below. | no (RAM, transient) | yes — banner + tone on receipt |
| `vol <1..32>` | Sidetone volume in `GAIN_Q15/1024` units (`8`→gain 8192, `32`→full swing 32768), clamped **1..32**. Default 8. Bare `vol` reports the current level. On the Heltec this scales the I2S sample amplitude; on the Cardputer it maps onto M5.Speaker 0..255. | yes | yes — applied immediately |
| `mute [on\|off]` | Sidetone mute. Bare `mute` toggles; `on`/`1` and `off`/`0` set explicitly. For a node running near people. | yes | yes — applied immediately |
| `keymode [compat\|edge]` | TX keying transport: `compat` = legacy 30 ms `KeyState` stream; `edge` = on-edge `EdgeEvent` packets carrying TX-measured durations (see `protocol.md` / `edge-events.md`). Bare `keymode` reports. Only the **fox/livekey TX** changes; hunters are bilingual and auto-follow per packet. Default `compat` (a fresh/legacy unit is unchanged). | yes | yes — fox/livekey emits the new format immediately |
| `msgmode [keyed\|text]` | **Fox.** Canned-message delivery: `keyed` (default) streams the clue as edge/compat keying; `text` sends the whole clue as one `MAGIC_TEXT` frame + retransmit burst and the **hunter renders Morse locally** (robust at the edge of range, where a single lost edge becomes a `?` — field note §7). Bare `msgmode` reports. Persisted; restored at boot. Applies to the canned fox loop only — live keying always keys edges. See `protocol.md` / `plan-text-message-mode.md`. | yes | yes — fox sends the new format next cycle |
| `debug [on\|off]` | Toggle a parseable serial dump of the live link for unattended testing: received packets (`RX E`/`RX K`/`RX I`), decoded elements (`EL`), and characters (`CH`); a fox also prints `TX E`. In `msgmode text` (field note §7/§8) a fox also prints `TX T` (clue burst) and `TX S` (sync beacon), and a hunter prints `RX T` (clue / `mid-join`), `RX S` (sync beacon: `pos`/`drift`/`seek`), and `TEL` (one per dit/dah as it is revealed). Bare `debug` toggles. **Not** persisted (RAM only). See `edge-events.md` "Unattended test instrumentation". | no (RAM) | yes |
| `stop` / `start` | **Fox/Live-key.** Halt or resume all air output (keystate/edge stream **and** the periodic Ident). Takes effect immediately at the TX point (no wait for the current element). Runtime-only — a reboot comes up transmitting. `show` reports `tx=running\|halted`. | no (RAM) | yes |
| `showtext [on\|off]` | **Hunter.** Copy-line view: `off` (default) = raw dit/dah elements, `on` = decoded text. Bare `showtext` toggles. Replaces the old PRG view-toggle (the button now cycles volume). | no (RAM) | yes |
| `show` | Print the current config: `id`, `call`, `wpm`, `farns`, `vol`, `mute`, boot `mode`, `keymode`, `msgmode`, `tx`, `msg`, `rxtext` (last text-frame a Hunter received: `"clue" seq elems rx` — `elems`>0 confirms local Morse render), plus `rxbw`, `ble`, and the active `banner` text if one is showing. | — | — |
| `batt` | Print the raw battery readout: terminal `mV`, smoothed `%`, and `charging`. Disambiguates a `0%` meter (no cell / flat pack vs a scaling problem). | — | — |
| `bootlog` | Dump the boot/crash-reason ring (last 16 boots, oldest first): `#<boot> reason=<n>(<name>)`. A watchdog reboot shows as `TASK_WDT` (ESP32) or `WATCHDOG` (nRF52) — see "Field watchdog" below. Diagnoses crashes after the fact with no serial attached at reset time. `bootlog clear` empties the ring (the monotonic boot counter is kept). | — | reads NVS |
| `screen [on\|off]` | Idle display-blanking (battery saver). Bare `screen`/`screen show` reports `idle <ms>/60000` and blanked state; `on` forces the panel awake + resets the idle timer; `off` blanks now. The panel also self-blanks after 60 s with no activity and wakes on any button/serial/BLE command (and inbound keying on a hunter). | no (RAM) | yes |
| `power` | Report the power/clock state for battery-saver verification: running CPU clock (`cpu = <n> MHz`, ESP32-S3 idles at 80 MHz), physical `cores`, and the same `screen`/idle line. Read-only. | — | reads state |
| `ble [on\|off\|auto]` | BLE-UART (NUS) power policy. `auto` (default) follows the panel — lit = advertising, idle-blanked = off — **except a Hunter**, where inbound RX keeps the panel lit indefinitely, so `auto` instead drops BLE 60 s after the last button/console/keyboard touch (a connected central is never dropped). `on`/`off` pin the core regardless of mode (e.g. reach an idle instructor). Instructors always boot pinned `on`. Bare `ble` / `show` reports state. Reclaims ~70 mA on the V4 when down. | no (RAM) | yes |
| `hibernate` | Remote power-down: SX1262 to sleep, then MCU deep sleep (same path as the boot-menu Hibernate item). Only a hardware RST — or a bench serial reconnect that pulses auto-reset — wakes the node. | — | yes — node sleeps |
| `stall [secs]` | **Resiliency self-test.** Deliberately wedge `loop()` by spinning **without feeding the hardware watchdog**, to prove a hung node actually reboots itself in the field. The 8 s watchdog fires first and you never return — next boot's `bootlog` shows the watchdog cause. Optional `secs` (default 30) caps the spin as a backstop; if it ends and prints `# stall ended — watchdog did NOT fire`, the watchdog is disarmed. Always compiled (unlike the debug-only `panic`). See "Field watchdog". | — | yes — wedges the running node |
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
  connect, tweak, disconnect, and reconnect throughout an exercise. The default
  `ble auto` policy powers the core down when idle to save battery — on a
  Hunter that's 60 s after the last button/console/keyboard touch, regardless
  of inbound RX (see the `ble` row above); use `ble on` to reach an idle node.
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

## Instructor (remote control over GFSK)

In the field the fox may sit far from the operator — beyond BLE range. The
**Instructor** boot mode (menu entry "Instructor (RC)", or `mode 3`) turns a node
(typically the Cardputer) into a remote that pushes the commands above to a
distant station over the **GFSK radio**, so an exercise leader can re-tune a fox
live (new clue text, faster speed, more power) without walking to it.

**TX power.** The instructor always starts at MAX so every fox in the field
hears the command bursts. The PRG button wakes the display only (no power
cycling). Use `ipwr <0..3>` to lower power for bench work; it is not persisted
and resets to MAX on the next boot. The OLED always shows the current TX level
and battery state in the header.

**How it's driven.** The instructor is itself reachable over its own BLE/serial
console; you type `relay <id> <cmd>` there and it relays `<cmd>` on-air:

```
relay 43 msg DE W1ABC FOX UNDER THE OAK   # change fox 43's message
relay 43 wpm 20                            # speed it up
relay 255 mute on                          # quiet every station
```

The target runs the command through the *same* `handle_setup_command()` parser as
BLE/serial (so it persists and live-applies identically) and returns a short
**ack**; the instructor shows `ACK <id>: <reply>` on its screen and serial.

**On-air mechanics.** Control rides in a dedicated packet type (`MAGIC_CTRL`,
`protocol.h`); the instructor is the reserved source id **0**, and a station only
honours control from id 0 addressed to its own id (or `255`, broadcast). Because
the radio is half-duplex and a fox is almost always transmitting, the fox opens a
short **receive window** (the silent tail of its inter-message pause, kept under
the hunter 3 s presence timeout). The instructor *silence-syncs* to a unicast
target — it watches the fox's own stream and bursts a command only when the fox
falls silent (its window), avoiding collisions with the keystate the hunters are
copying. It keeps trying for up to ~90 s and stops as soon as the target acks.
Broadcast (`255`) has no single phase to track, so it falls back to a sparse
periodic probe and collects acks from everyone for the window.

**Caveats.** Same open trust model as BLE (any node on the channel could forge id
0 — fine for a closed exercise). Broadcast delivery is best-effort and noisier on
air than unicast. The full command set is reachable remotely, including `reboot`
and `mode` (a `reboot` won't ack — the target resets first). Live-key stations
don't answer control in this version.

## Instructor alert (banner + tone to every station)

`alert` pushes a short **human-readable banner** to the screen of every station
in the exercise, regardless of mode — for "RETURN TO BASE", "FOX QSY 906.000", a
hint, or a safety call — **and sounds an attention tone** so an operator who
isn't looking at the panel still notices. It is distinct from `relay`: `relay`
carries a *console command* a station runs silently; `alert` carries *display
text* shown verbatim, with audio.

```
alert RETURN TO BASE NOW        # banner + ~1.5 s tone on every Fox/Hunter/Instructor panel
alert FOX QSY 906.000           # re-point hunters to a new frequency; force-wakes blanked panels
alert NET IN 5 MIN ON 905.0     # short text → centered (wraps to two lines if it fits)
alert SHELTER IN PLACE - HEAVY RAIN MOVING IN, HOLD POSITION   # too long → marquee-scrolls
alert WX ALERT                  # beeps even on a node with `mute on` (mute restored after)
alert clear                     # dismiss the banner on every station (silent, no tone)
```

**Audible attention tone.** Every alert (with text) sounds a single ~1.5 s tone
at the board's sidetone frequency. The tone **overrides the master `mute`** — a
broadcast is important enough to be heard even on a node muted near people — but
it does *not* change the persisted mute state, so the node is muted again the
instant the tone ends. Implemented as a mute-bypassing `sidetone_alert()` gate in
every audio backend (I2S/MAX98357A, Cardputer ES8311, nRF52 buzzer); the main
loop times the duration without blocking. `alert clear` is silent.

**On-air mechanics.** The banner rides a dedicated packet (`MAGIC_BCAST`,
`protocol.h`), again honoured only from instructor id **0**, with the
`BCAST_FLAG_ALERT` bit always set (every broadcast is now an alert). It is always
all-stations and **fire-and-forget** — acking from every node would flood the
channel — so reliability comes from repetition: the instructor bursts the same
`seq` five times ~1.5 s apart (~7.5 s total), and receivers dedup by `seq` so the
banner paints and the tone fires once. Each station shows the text as a
full-screen overlay for **1 minute** over its normal mode screen (keying/copy
continues underneath), then reverts. The text is centered if short, wrapped to
two lines if it fits, or horizontally **scrolled** (marquee) if it is too long
for the panel. A banner **always force-wakes a blanked panel** and holds it lit
for the full minute (a broadcast must be seen, so it overrides idle-blank
regardless of the operator's blank state). A button tap / keypress on a station
dismisses its banner early. Older firmware that predates `MAGIC_BCAST` ignores
the packet for free (every decoder is magic-gated).

## Field watchdog

Every node arms a **hardware watchdog** so a wedged unit reboots itself instead of
going silently dead in the field. It is armed for **8 s** at the end of `setup()`
and fed at the top of `loop()`; if `loop()` stalls past 8 s the chip resets. The
implementation lives behind the `platform::` seam:

- **ESP32** (Heltec, Cardputer): the FreeRTOS **Task WDT** (`esp_task_wdt`) on the
  Arduino `loopTask`. A timeout is reported by `esp_reset_reason()` as `TASK_WDT`.
- **nRF52** (Wio Tracker, RAK4631): the **`NRF_WDT`** peripheral, clocked off the
  always-on 32.768 kHz LFCLK so it keeps counting even if the CPU, the SoftDevice,
  or an ISR wedges. The Adafruit bootloader clears the hardware `RESETREAS` (and
  wipes `.noinit` RAM) before the app runs, so the reason is instead recovered
  from a persisted flash **reboot-intent** flag: every boot re-arms it to
  `RUNNING`, and a clean `reboot`/`hibernate` stamps `SOFT`/`OFF` first — so an
  *unexpected* reset (watchdog, crash, brownout) is the only path that boots with
  `RUNNING` still set, reported as `WATCHDOG`. A bare power-cycle therefore also
  reads `WATCHDOG` (it too is "unexpected"); only `reboot` and `hibernate` are
  distinguished.

**Testing it.** Send `stall` (see the command table) to wedge `loop()` on purpose;
the watchdog should reset the node within ~8 s, and the next boot's `bootlog` shows
the watchdog cause. `scripts/watchdog_test.py <port>` automates the whole round
trip (clear ring → `stall` → confirm the USB port re-enumerates → read `bootlog`)
and exits non-zero if the watchdog did not fire. Hardware-validated on the Heltec
boards (ESP32) and the Wio Tracker L1 (nRF52: reset at 8.3 s → `reason=2(WATCHDOG)`).

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
- `scripts/watchdog_test.py` — fire `stall` and confirm the hardware watchdog
  reboots the node (PASS/FAIL exit code). See "Field watchdog".
- `tools/ble_provision_test.py` — bleak-based round-trip test of the BLE NUS.
