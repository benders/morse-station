#!/usr/bin/env python
# Send a setup-console command to a MorseStn-NN over the always-on BLE NUS and
# print the reply. Reuses the UUIDs/pattern from devices.py. No reset, no USB.
#   tools/blevenv/bin/python scripts/ble_cmd.py 73 "bootlog"
import asyncio, re, sys
from bleak import BleakClient, BleakScanner

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # write  (host -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # notify (device -> host)

async def main(target_id: str, cmd: str, scan_timeout=6.0, reply_timeout=5.0):
    want = f"MorseStn-{target_id}"
    found = {}
    def on_detect(dev, adv):
        name = adv.local_name or dev.name or ""
        if name == want or (target_id == "*" and name.startswith("MorseStn-")):
            found[dev.address] = (name, dev)
    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    for _ in range(int(scan_timeout * 4)):
        if found: break
        await asyncio.sleep(0.25)
    await scanner.stop()
    if not found:
        print(f"!! no {want} advertising (scanned {scan_timeout}s) — "
              f"device off, or looping before it advertises", file=sys.stderr)
        return 2
    addr, (name, dev) = sorted(found.items())[0]
    print(f"# connecting {name} [{addr}] -> {cmd!r}")
    received = bytearray()
    last = {"t": 0.0}
    loop = asyncio.get_event_loop()
    def on_notify(_c, data: bytearray):
        received.extend(data); last["t"] = loop.time()
    async with BleakClient(dev) as client:
        await client.start_notify(NUS_TX, on_notify)
        await client.write_gatt_char(NUS_RX, (cmd + "\n").encode(), response=False)
        # idle-gap close: stop once notifications stop arriving for 1.2s
        last["t"] = loop.time(); start = loop.time()
        while loop.time() - start < reply_timeout:
            await asyncio.sleep(0.2)
            if received and loop.time() - last["t"] > 1.2:
                break
        await client.stop_notify(NUS_TX)
    print("----- reply -----")
    print(received.decode("utf-8", "replace").rstrip())
    return 0

if __name__ == "__main__":
    tid = sys.argv[1] if len(sys.argv) > 1 else "*"
    command = sys.argv[2] if len(sys.argv) > 2 else "bootlog"
    raise SystemExit(asyncio.run(main(tid, command)))
