#!/usr/bin/env python3
"""List connected morse-station boards and their station IDs.

For every /dev/cu.usbmodem* device this reports the serial port, the board type,
the MAC, and the **live** station id + callsign read from the firmware's boot
banner. The live read matters because a unit's station id may be provisioned in
NVS to something other than the MAC-derived default (the two Heltec hunters are),
so the default alone is not trustworthy — see docs/.

How each field is obtained:
  * port + MAC  -- from `ioreg` (the ESP32-S3 USB serial number IS the MAC); no
                   reset, instant.
  * board type  -- from the flash size via `esptool flash_id` (8MB = Cardputer
                   ADV / cardputer_adv, 16MB = Heltec V4 / heltec_v4).
  * station id  -- the firmware prints `# config: id=.. call=.. ...` at boot, so
                   we reset the board (RTS pulse) and read that line. Falls back
                   to the MAC default (flagged UNVERIFIED) if the window is missed.

The MAC default id is `(XOR of the 6 MAC bytes) % 254 + 1` (config.cpp's
mac_station_id). NOTES flags whether the live id matches that default.

Modes (all read the same live id/call/mode/mute; they differ in transport):
    scripts/devices.sh              # --usb (default): reset-free live read over
                                     #   the runtime serial console; maps to USB
                                     #   port + MAC. Non-disruptive.
    scripts/devices.sh --ble        # reset-free over always-on BLE, but NOT
                                     #   mapped to USB ports (macOS hides the
                                     #   BLE MAC)
    scripts/devices.sh --boot       # resets each board (~10s/board) and reads
                                     #   via the boot setup console; adds the
                                     #   BOARD column. Disruptive; for older
                                     #   firmware without the runtime console.

The default (--usb) and --ble are both non-disruptive: a running station answers
`show` without rebooting (over USB serial or BLE NUS respectively). --boot is the
old default — it resets each board, so it interrupts a running fox/exercise, but
it is the fallback for firmware predating the runtime serial console and is the
only mode that reports the BOARD type (needs a bootloader reset).
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import time

import serial  # provided by the PlatformIO venv

ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")


def parse_show(text: str) -> dict:
    """Parse a firmware `show` line: id/call/wpm/farns/mute/mode (mute & mode are
    only present on firmware that supports them — older units omit them)."""
    m = re.search(r"id=(\d+) call=(\S+) wpm=(\d+) farns=(\d+)"
                  r"(?: mute=(\w+))?(?: mode=(\w+))?", text)
    if not m:
        return {}
    out = {"id": int(m.group(1)), "call": m.group(2),
           "wpm": int(m.group(3)), "farns": int(m.group(4))}
    if m.group(5):
        out["mute"] = m.group(5)
    if m.group(6):
        out["mode"] = m.group(6)
    return out


def mac_default_id(mac: str) -> int:
    """The firmware's default station id: XOR the 6 MAC bytes, fold to 1..254."""
    x = 0
    for byte in mac.split(":"):
        x ^= int(byte, 16)
    return (x % 254) + 1


def port_mac_map() -> dict[str, str]:
    """Map /dev/cu.usbmodem* -> MAC via ioreg. The Espressif USB serial number is
    the MAC; the serial tty (IOCalloutDevice) is nested under that USB device, so
    a callout belongs to the nearest enclosing (shallower-indented) serial number.
    """
    try:
        raw = subprocess.run(["ioreg", "-l", "-w", "0"],
                             capture_output=True, check=True).stdout
        out = raw.decode("utf-8", "replace")   # ioreg emits stray non-UTF8 bytes
    except Exception as e:
        sys.stderr.write(f"warning: ioreg failed ({e}); cannot read MACs\n")
        return {}

    mapping: dict[str, str] = {}
    stack: list[tuple[int, str]] = []   # (indent, mac) ancestor chain
    for line in out.splitlines():
        q = line.find('"')
        if q < 0:
            continue
        indent = q
        m = re.search(r'"USB Serial Number" = "([0-9A-Fa-f:]+)"', line)
        if m and MAC_RE.match(m.group(1).upper()):
            mac = m.group(1).upper()
            while stack and stack[-1][0] >= indent:   # keep a clean ancestor stack
                stack.pop()
            stack.append((indent, mac))
            continue
        c = re.search(r'"IOCalloutDevice" = "(/dev/cu\.usbmodem[^"]+)"', line)
        if c:
            owner = None
            for ind, mac in stack:                    # deepest ancestor wins
                if ind < indent:
                    owner = mac
            if owner:
                mapping[c.group(1)] = owner
    return mapping


def flash_board(port: str) -> tuple[str, str]:
    """Return (flash_size, board_label) via esptool flash_id. Resets the board."""
    py = sys.executable   # the PlatformIO venv python running this script
    try:
        out = subprocess.run([py, ESPTOOL, "--port", port, "flash_id"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return ("?", "?")
    m = re.search(r"Detected flash size:\s*(\S+)", out)
    size = m.group(1) if m else "?"
    board = {"8MB": "Cardputer (cardputer_adv)",
             "16MB": "Heltec V4 (heltec_v4)"}.get(size, "?")
    return (size, board)


def pulse_reset(s: serial.Serial) -> None:
    """RTS reset — the sequence esptool uses ('Hard resetting via RTS pin')."""
    try:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)
    except Exception:
        pass


def wait_for_port(path: str, timeout: float = 6.0) -> str | None:
    """The native-USB CDC drops on reset and re-enumerates; wait for it back.
    Prefer the original name, else any usbmodem that reappears."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ports = glob.glob("/dev/cu.usbmodem*")
        if path in ports:
            return path
        if ports:
            return sorted(ports)[0]
        time.sleep(0.2)
    return None


def read_station(port: str, timeout: float = 18.0) -> dict:
    """Reset the board and read its live settings via the boot setup console.

    The `# config:` banner prints inside the firmware's brief 500 ms `while(!Serial)`
    window — before macOS finishes re-enumerating the native-USB CDC after a reset,
    so it is reliably missed. The setup console's 's' prompt, by contrast, stays
    open for ~1 s, which IS catchable: we hammer 's' until the `setup>` prompt
    appears, then issue `show` (read-only) and parse its id/call/mute line.
    """
    info: dict = {}
    try:
        s = serial.Serial(port, 115200, timeout=0.1)
    except Exception:
        return info
    pulse_reset(s)
    s.close()
    time.sleep(0.5)
    port = wait_for_port(port) or port
    try:
        s = serial.Serial(port, 115200, timeout=0.1)
    except Exception:
        return info

    buf = ""
    in_repl = False
    last_s = 0.0
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not in_repl and time.time() - last_s > 0.1:
            try:
                s.write(b"s\r\n")    # land inside the 1 s setup window
            except Exception:
                pass
            last_s = time.time()
        chunk = s.read(256).decode("utf-8", "replace")
        if chunk:
            buf += chunk
        b = re.search(r"# morse-station build (\S+)", buf)
        if b:
            info["build"] = b.group(1)
        if not in_repl and "setup>" in buf:
            in_repl = True
            time.sleep(0.2)
            try:
                s.reset_input_buffer()
            except Exception:
                pass
            s.write(b"show\r\n")     # read-only; changes nothing
            buf = ""
        # `show` prints: id=<n> call=<C> wpm=<w> farns=<f> mute=<on|off> mode=<M> msg="..."
        if in_repl:
            parsed = parse_show(buf)
            if parsed:
                info.update(parsed)
                break
    try:
        s.write(b"done\r\n")         # leave the console; resume normal boot
    except Exception:
        pass
    s.close()
    return info


def read_station_live(port: str, timeout: float = 3.0) -> dict:
    """Read live settings over the *runtime* serial console — no reset.

    The firmware polls Serial for commands inside loop() (the sibling of the BLE
    console — see serial_console_process in main.cpp), so a running station
    answers `show` without rebooting. Opening the USB-JTAG/serial CDC does not
    reset the ESP32-S3 (the HWCDC unit has no auto-reset on DTR/RTS — esptool
    resets it with a dedicated USB-JTAG command instead), so this is
    non-disruptive: the USB analogue of --ble, but mappable to a port/MAC.

    A station parked in the boot menu (pre-loop) won't answer; we resend `show`
    across the window so one that auto-selects mid-read is still caught. If it
    never replies, the caller falls back to the MAC default and flags it.
    """
    info: dict = {}
    try:
        s = serial.Serial(port, 115200, timeout=0.1)
    except Exception:
        return info
    try:
        s.reset_input_buffer()
        buf = ""
        last_send = 0.0
        deadline = time.time() + timeout
        while time.time() < deadline:
            if time.time() - last_send > 0.5:
                try:
                    s.write(b"show\r\n")     # read-only; changes nothing
                except Exception:
                    break
                last_send = time.time()
            chunk = s.read(256).decode("utf-8", "replace")
            if chunk:
                buf += chunk
                parsed = parse_show(buf)
                if parsed:
                    info.update(parsed)
                    break
    finally:
        s.close()
    return info


# --- BLE mode: read live station ids over the always-on NUS, no reset --------
# Nordic UART Service UUIDs (must match src/ble_provision.cpp).
NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # write  (host -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # notify (device -> host)


async def _ble_show(client, reply_timeout: float) -> dict:
    """Run the read-only `show` command over NUS and parse the reply."""
    import asyncio
    received = bytearray()
    done = asyncio.Event()

    def on_notify(_char, data: bytearray):
        received.extend(data)
        if b"\n" in received:
            done.set()

    await client.start_notify(NUS_TX, on_notify)
    await client.write_gatt_char(NUS_RX, b"show\n", response=False)
    try:
        await asyncio.wait_for(done.wait(), timeout=reply_timeout)
    except asyncio.TimeoutError:
        pass
    await client.stop_notify(NUS_TX)

    return parse_show(received.decode("utf-8", "replace"))


async def _ble_collect(scan_timeout: float, enrich: bool, reply_timeout: float) -> list:
    from bleak import BleakClient, BleakScanner
    import asyncio

    seen: dict = {}

    def on_detect(dev, adv):
        name = adv.local_name or dev.name or ""
        if name.startswith("MorseStn-"):
            seen[dev.address] = (name, dev)

    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    await asyncio.sleep(scan_timeout)
    await scanner.stop()

    results = []
    for addr, (name, dev) in sorted(seen.items()):
        m = re.search(r"MorseStn-(\d+)", name)
        entry = {"name": name, "addr": addr,
                 "id": int(m.group(1)) if m else None}
        if enrich:
            try:
                async with BleakClient(dev) as client:
                    entry.update(await _ble_show(client, reply_timeout))
            except Exception as e:
                entry["err"] = type(e).__name__
        results.append(entry)
    return results


def ble_mode(scan_timeout: float, enrich: bool, reply_timeout: float) -> int:
    """List stations on the air via always-on BLE advertising — no reset, no USB.

    macOS CoreBluetooth hides the BLE MAC (see cardputer-adv-port notes), so this
    CANNOT be joined to a /dev/cu.usbmodem* port; it answers "which stations are
    live and what are their settings", non-disruptively, while an exercise runs.
    """
    try:
        import asyncio
        import bleak  # noqa: F401  (import check)
    except Exception:
        sys.stderr.write("error: --ble needs bleak; run via tools/blevenv/bin/python\n")
        return 1
    rows = asyncio.run(_ble_collect(scan_timeout, enrich, reply_timeout))
    print(f"\n# stations on air via BLE ({len(rows)} found) — reset-free, not USB-mapped")
    if not rows:
        print("  (none — is BLE advertising up? try --scan-timeout 15)")
        return 0
    headers = ("STATION", "CALLSIGN", "MODE", "MUTE", "BLE NAME", "BLE ADDRESS")
    data = [(str(r.get("id", "?")), r.get("call", r.get("err", "?")),
             r.get("mode", "?"), r.get("mute", "?"), r["name"], r["addr"])
            for r in rows]
    cols = list(zip(headers, *data))
    widths = [max(len(str(c)) for c in col) for col in cols]
    fmt = "  ".join("{:<%d}" % w for w in widths)
    print(fmt.format(*headers))
    for row in data:
        print(fmt.format(*row))
    return 0


def _print_table(headers: tuple, rows: list) -> None:
    """Render an aligned fixed-width table."""
    cols = list(zip(headers, *rows)) if rows else [(h,) for h in headers]
    widths = [max(len(str(c)) for c in col) for col in cols]
    fmt = "  ".join("{:<%d}" % w for w in widths)
    print(fmt.format(*headers))
    for row in rows:
        print(fmt.format(*row))


def _classify(station, default_id: int | None, got_reply: bool) -> str:
    """NOTES text: how the read id relates to the MAC default."""
    if not got_reply:
        return "no reply (running? at boot menu? try --boot)"
    if default_id is None:
        return "live"
    if station == default_id:
        return "default"
    return f"provisioned (MAC default {default_id})"


def usb_mode() -> int:
    """Default: read live id/mode/mute over the runtime serial console — no reset.

    The USB analogue of --ble: non-disruptive (a running station answers `show`
    without rebooting), but unlike BLE it maps to /dev/cu.usbmodem* + MAC via
    ioreg. No BOARD column — flash-size detection needs a bootloader reset, which
    only --boot does.
    """
    macs = port_mac_map()
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        print("no /dev/cu.usbmodem* devices found")
        return 1

    rows = []
    for port in ports:
        mac = macs.get(port, "?")
        default_id = mac_default_id(mac) if MAC_RE.match(mac) else None
        print(f"# reading {port} (live, no reset) ...", file=sys.stderr)
        info = read_station_live(port)
        station = info.get("id", default_id)
        note = _classify(station, default_id, "id" in info)
        rows.append((port, mac,
                     str(station) if station is not None else "?",
                     info.get("call", "?"), info.get("mode", "?"),
                     info.get("mute", "?"), note))

    print(f"\n# morse-station devices ({len(rows)} found) — live over USB, no reset")
    _print_table(("PORT", "MAC", "STATION", "CALLSIGN", "MODE", "MUTE", "NOTES"), rows)
    return 0


def boot_mode(no_flash: bool) -> int:
    """Reset each board and read its settings via the boot setup console.

    Slower and disruptive (a reset interrupts a running fox/exercise), but it is
    the fallback for firmware too old to have the runtime serial console, and it
    adds the BOARD column from `esptool flash_id`.
    """
    macs = port_mac_map()
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        print("no /dev/cu.usbmodem* devices found")
        return 1

    rows = []
    for port in ports:
        mac = macs.get(port, "?")
        default_id = mac_default_id(mac) if MAC_RE.match(mac) else None

        board = "?"
        if not no_flash:
            print(f"# probing {port} ...", file=sys.stderr)
            _, board = flash_board(port)

        print(f"# reading station id on {port} (resetting) ...", file=sys.stderr)
        info = read_station(port)
        got = "id" in info
        station = info.get("id", default_id)
        note = (_classify(station, default_id, True) if got
                else "UNVERIFIED — console window missed; showing MAC default")
        rows.append((port, board, mac,
                     str(station) if station is not None else "?",
                     info.get("call", "?"), info.get("mode", "?"),
                     info.get("mute", "?"), note))

    print(f"\n# morse-station devices ({len(rows)} found) — boot read, resets each board")
    _print_table(("PORT", "BOARD", "MAC", "STATION", "CALLSIGN", "MODE", "MUTE", "NOTES"),
                 rows)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="List morse-station boards and station IDs.")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--usb", action="store_true",
                   help="(default) reset-free live read over the runtime serial "
                        "console; maps to USB port + MAC")
    g.add_argument("--boot", action="store_true",
                   help="reset each board and read via the boot setup console; "
                        "adds the BOARD column. Use for older firmware. Disruptive")
    g.add_argument("--ble", action="store_true",
                   help="reset-free: scan always-on BLE for live station ids "
                        "(no USB port mapping — macOS hides the BLE MAC)")
    p.add_argument("--no-flash", action="store_true",
                   help="--boot: skip esptool board detection (one fewer reset per board)")
    p.add_argument("--no-enrich", action="store_true",
                   help="--ble: list advertised ids only; skip the per-device NUS show")
    p.add_argument("--scan-timeout", type=float, default=8.0,
                   help="--ble: BLE scan duration in seconds (default 8)")
    p.add_argument("--reply-timeout", type=float, default=4.0,
                   help="--ble: per-device NUS show reply timeout (default 4)")
    args = p.parse_args()

    if args.ble:
        return ble_mode(args.scan_timeout, not args.no_enrich, args.reply_timeout)
    if args.boot:
        return boot_mode(args.no_flash)
    return usb_mode()


if __name__ == "__main__":
    sys.exit(main())
