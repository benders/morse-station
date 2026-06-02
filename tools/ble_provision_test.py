#!/usr/bin/env python3
"""macOS-side smoke test for the BLE field-provisioning NUS service.

Confirms a morse-station unit advertises a Nordic-UART service at boot and that
the existing provisioning parser (handle_setup_command) round-trips over BLE:
write a command to the RX characteristic, read the response back over TX-notify.

The firmware tears BLE down right before radio init, so advertising is only live
for a short boot window (~8 s). This script optionally pulses the board's reset
over its USB serial port to (re)open that window immediately before scanning, so
the test is self-synchronising. On macOS CoreBluetooth hides the BLE MAC, so we
match on the advertised name (default prefix "MorseStn-").

Usage:
    tools/blevenv/bin/python tools/ble_provision_test.py \
        --serial /dev/cu.usbmodem20101 --name MorseStn-42 --command show

Exit code 0 on a verified round-trip, non-zero otherwise.
"""
import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

# Nordic UART Service UUIDs (must match src/ble_provision.cpp).
NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write  (phone -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify (device -> phone)


def pulse_reset(port: str) -> None:
    """Pulse EN low via DTR/RTS to restart the sketch (ESP32 auto-reset)."""
    import serial  # pyserial; only needed when --serial is given

    s = serial.Serial(port, 115200, timeout=0.2)
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)
    time.sleep(0.1)
    s.close()


async def find_device(args):
    """Run a continuous scan and catch the unit's short boot advertising window.

    CoreBluetooth needs a moment to power on and start scanning, and the firmware
    only advertises for ~8 s after boot, so we start the scanner FIRST and pulse
    reset once it is live — otherwise scanner warmup misses the window.
    """
    found = asyncio.Event()
    hit = {}

    def on_detect(dev, adv):
        adv_name = adv.local_name or dev.name or ""
        if (args.name and adv_name == args.name) or (
            not args.name and adv_name.startswith(args.name_prefix)
        ):
            hit["dev"], hit["name"] = dev, adv_name
            found.set()

    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    # Scanner is now live; reopen the boot window so we catch it as it appears.
    if args.serial:
        print(f"[reset] pulsing {args.serial} to reopen boot advertising window")
        try:
            pulse_reset(args.serial)
        except Exception as e:  # noqa: BLE001 - report; passive scan may still catch it
            print(f"[reset] warning: could not reset over serial: {e}")
    try:
        await asyncio.wait_for(found.wait(), timeout=args.scan_timeout)
    except asyncio.TimeoutError:
        pass
    finally:
        await scanner.stop()
    return hit.get("dev"), hit.get("name")


async def run(args) -> int:
    target = args.name or f"(prefix {args.name_prefix!r})"
    print(f"[scan] scanning for {target} ...")
    dev, adv_name = await find_device(args)
    if dev is None:
        print(f"[scan] FAIL: no advertiser matched within {args.scan_timeout:.0f}s")
        return 2
    print(f"[scan] found '{adv_name}' ({dev.address})")

    received = bytearray()
    done = asyncio.Event()

    def on_notify(_char, data: bytearray):
        received.extend(data)
        # handle_setup_command terminates the "show" line with '\n'.
        if b"\n" in received:
            done.set()

    print(f"[conn] connecting ...")
    async with BleakClient(dev) as client:
        if not client.is_connected:
            print("[conn] FAIL: did not connect")
            return 3
        svcs = {s.uuid.lower() for s in client.services}
        if NUS_SVC not in svcs:
            print(f"[conn] FAIL: NUS service {NUS_SVC} not present; saw {sorted(svcs)}")
            return 4
        print("[conn] connected, NUS present; subscribing to TX-notify")
        await client.start_notify(NUS_TX, on_notify)

        payload = (args.command + "\n").encode()
        print(f"[tx]   writing {args.command!r}")
        await client.write_gatt_char(NUS_RX, payload, response=False)

        try:
            await asyncio.wait_for(done.wait(), timeout=args.reply_timeout)
        except asyncio.TimeoutError:
            print(f"[rx]   FAIL: no reply within {args.reply_timeout:.0f}s")
            return 5
        await client.stop_notify(NUS_TX)

    reply = received.decode("utf-8", "replace").strip()
    print(f"[rx]   reply: {reply}")
    print("[ok]   BLE provisioning round-trip verified")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--name", help="exact advertised name to match (e.g. MorseStn-42)")
    ap.add_argument("--name-prefix", default="MorseStn-",
                    help="prefix to match when --name is omitted (default MorseStn-)")
    ap.add_argument("--serial", help="USB serial port to pulse-reset before scanning")
    ap.add_argument("--command", default="show", help="provisioning command to send")
    ap.add_argument("--scan-timeout", type=float, default=20.0)
    ap.add_argument("--reply-timeout", type=float, default=5.0)
    args = ap.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
