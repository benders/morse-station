#!/usr/bin/env python
"""Automated validation of the BLE-attached instructor ACK gap fix (#3).

Reproduces the 2026-06-18 failure and checks the multi-ACK fix flips it.

THE TRAP: you cannot measure this off the BLE notify stream. A *separate*,
already-tracked bug (BLE notify throughput, TODO.md "BLE notify throughput —
long replies drop chunks") shreds the ACK text down to a bare newline when a
phone is attached, so the ACK looks absent over BLE even when it arrived. The
instructor prints every received ACK to USB serial too (`Serial.printf` in
instructor_service_rx), so the honest measurement is: keep a BLE central
ATTACHED (this is what reproduces the connection-event jitter that caused #3)
and watch the instructor's USB serial for `ACK <tgt> seq=`.

  T1/T4  BLE-attached relay -> ACK on instructor serial   (the #3 fix; was 0/4)
  T3     USB relay, NO BLE central                        (control: still 4/4)
  T5     all ACK copies for a relay carry ONE seq         (dedup = one logical ack)
  T2     `show` readback on each target over USB          (delivery actually landed)

Run from the repo root with the BLE venv (bleak + pyserial):
  tools/blevenv/bin/python scripts/ble_ack_test.py [--setup]
--setup first puts stn73 into Instructor mode (mode 3 + reboot).
"""
import argparse, asyncio, re, threading, time
import serial
from bleak import BleakClient, BleakScanner

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # write  (host -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # notify (device -> host)

INSTRUCTOR_ID   = 73
INSTRUCTOR_PORT = "/dev/cu.usbmodem20101"
TARGETS = {                       # id -> (usb port, is_nrf52)
    43:  ("/dev/cu.usbmodem20401", False),   # Heltec V4 Hunter
    115: ("/dev/cu.usbmodem21301", True),    # Wio Tracker L1 Hunter (nRF52)
}
N = 4
ACK_WAIT = 7.0
PASS_RATIO = 3                    # >= this many of N must ACK to PASS

results = []
def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""),
          flush=True)


# ---------- a background reader for one serial port ----------
class SerialTap:
    def __init__(self, port, nrf52=False, settle=2.0):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        if nrf52:
            self.s.dtr = True
        time.sleep(settle); self.s.reset_input_buffer()
        self.log = []                     # (t, line)
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True); self._t.start()
    def _run(self):
        buf = bytearray()
        while not self._stop:
            buf.extend(self.s.read(256))
            while b"\n" in buf:
                ln, _, rest = buf.partition(b"\n")
                self.log.append((time.time(), ln.decode("utf-8", "replace").rstrip()))
                buf[:] = rest
    def cmd(self, text, read_secs=4.0):
        m0 = len(self.log)
        self.s.write((text + "\n").encode()); self.s.flush()
        time.sleep(read_secs)
        return [ln for _, ln in self.log[m0:]]
    def close(self):
        self._stop = True; time.sleep(0.3); self.s.close()


async def find_instructor(scan_timeout=8.0):
    want = f"MorseStn-{INSTRUCTOR_ID}"
    found = {}
    def on_detect(dev, adv):
        if (adv.local_name or dev.name or "") == want:
            found[dev.address] = dev
    sc = BleakScanner(detection_callback=on_detect)
    await sc.start()
    for _ in range(int(scan_timeout * 4)):
        if found: break
        await asyncio.sleep(0.25)
    await sc.stop()
    return sorted(found.items())[0][1] if found else None


async def ble_attached_phase(tap):
    # One BLE connection held across all relays (= the field phone), driving the
    # instructor while we read its serial for the ACKs it catches over the air.
    dev = await find_instructor()
    if dev is None:
        for tid, label in ((43, "T1"), (115, "T4")):
            record(f"{label} BLE-attached relay to {tid}", False,
                   "instructor not advertising")
        return
    async with BleakClient(dev) as client:
        await client.start_notify(NUS_TX, lambda _c, _d: None)   # subscribe (jitter source)
        for tid, label in ((43, "T1"), (115, "T4")):
            hits, seqs_ok = 0, 0
            for i in range(N):
                val = 12 + i
                m0 = len(tap.log)
                await client.write_gatt_char(
                    NUS_RX, f"relay {tid} wpm {val}\n".encode(), response=False)
                t0 = time.time(); acks = []
                while time.time() - t0 < ACK_WAIT:
                    await asyncio.sleep(0.2)
                    acks = [ln for _, ln in tap.log[m0:]
                            if re.search(rf"ACK {tid} seq=", ln)]
                    if acks: break
                # let the rest of the multi-ACK copies land before tallying dedup
                await asyncio.sleep(0.8)
                acks = [ln for _, ln in tap.log[m0:] if re.search(rf"ACK {tid} seq=", ln)]
                seqs = {re.search(rf"ACK {tid} seq=(\d+)", a).group(1) for a in acks}
                hits += 1 if acks else 0
                seqs_ok += 1 if len(seqs) == 1 else 0
                print(f"    relay {tid} wpm {val}: {len(acks)} ACK copy(s), "
                      f"seq(s)={sorted(seqs)}", flush=True)
            record(f"{label} BLE-attached relay to {tid} -> ACK received",
                   hits >= PASS_RATIO, f"{hits}/{N} relays acked (pre-fix 0/{N})")
            if tid == 43:
                record("T5 each relay's ACK copies share one seq (dedup)",
                       seqs_ok >= PASS_RATIO, f"{seqs_ok}/{N} relays single-seq")
        await client.stop_notify(NUS_TX)


def usb_control_phase():
    # No BLE central attached -> the known-good path. Must stay 4/4.
    tap = SerialTap(INSTRUCTOR_PORT)
    try:
        tid, hits = 43, 0
        for i in range(N):
            val = 22 + i
            lines = tap.cmd(f"relay {tid} wpm {val}", read_secs=ACK_WAIT)
            ok = any(re.search(rf"ACK {tid} seq=", ln) for ln in lines)
            hits += 1 if ok else 0
            print(f"    [usb] relay {tid} wpm {val}: {'ACK' if ok else 'NO ACK'}",
                  flush=True)
        record("T3 USB relay still 4/4 (no BLE)", hits == N, f"{hits}/{N} acks")
    finally:
        tap.close()


def delivery_readback():
    # Last distinct, un-clamped value relayed to each target: 43 got USB wpm 25,
    # 115 got BLE wpm 15 (set_wpm clamps at 40, so these stay distinct).
    expect = {43: 25, 115: 15}
    for tid, (port, nrf52) in TARGETS.items():
        try:
            tap = SerialTap(port, nrf52=nrf52)
            lines = tap.cmd("show", read_secs=4.0); tap.close()
        except Exception as e:
            record(f"T2 delivery readback stn{tid}", False, f"serial error: {e}")
            continue
        got = None
        for ln in lines:
            m = re.search(r"wpm\s*=?\s*(\d+)", ln)
            if m: got = int(m.group(1))
        record(f"T2 delivery readback stn{tid}", got == expect[tid],
               f"wpm={got} (expected {expect[tid]})")


async def amain(args):
    if args.setup:
        print("# stn73 -> Instructor mode + reboot", flush=True)
        tap = SerialTap(INSTRUCTOR_PORT)
        print("   " + " | ".join(tap.cmd("mode 3", read_secs=2.0))[-100:], flush=True)
        tap.cmd("reboot", read_secs=1.0); tap.close()
        time.sleep(16)

    print("\n== T1/T4/T5: relay over BLE (phone attached), ACK read on serial ==", flush=True)
    tap = SerialTap(INSTRUCTOR_PORT)
    await ble_attached_phase(tap)
    tap.close()

    print("\n== T3: relay over USB (no BLE control) ==", flush=True)
    usb_control_phase()

    print("\n== T2: delivery readback ==", flush=True)
    delivery_readback()

    n = sum(1 for _, ok, _ in results if ok)
    print(f"\n===== {n}/{len(results)} checks PASS =====", flush=True)
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--setup", action="store_true",
                    help="put stn73 into Instructor mode (mode 3 + reboot) first")
    raise SystemExit(asyncio.run(amain(ap.parse_args())))
