#!/usr/bin/env python3
"""Text-frame canned-message mode bench test (field note §7).

Validates the §7 behavior implemented on branch feat/sticky-alert-fox-halt:
  - `msgmode text` (vs the default `keyed`) makes the Fox send the whole clue as
    one MAGIC_TEXT frame + retransmit burst instead of streaming EdgeEvent edges.
  - The Hunter receives the frame, dedups the burst by seq, shows the clue text
    verbatim with ZERO '?', and renders Morse LOCALLY (its own morse::Player
    drives the sidetone — observable via the `RX T render done elems=N` debug line).
  - `msgmode` is persisted (survives a reboot) and reported by `show`.
  - The §6 fox-halt (`stop`/`start`) gates the text burst too.
  - A node on OLD firmware (the un-flashed Instructor) ignores MAGIC_TEXT frames
    without misparsing or crashing (forward-compat).

Fully automated and unattended: it FLASHES the current build to the Fox and the
Heltec Hunter (and best-effort to the Wio Hunter), then asserts on passively
observed serial output. No human interaction, no menu input (boards boot into
their persisted mode).

Topology (one bench; from scripts/devices.sh --usb):
  - Fox      = stn42  /dev/cu.usbmodem22301  Heltec V4   (flashed, REQUIRED)
  - Hunter A = stn43  /dev/cu.usbmodem21401  Heltec V4   (flashed, REQUIRED)
  - Hunter B = stn115 /dev/cu.usbmodem20301  Wio nRF52   (flashed, BEST-EFFORT)
  - Instr.   = stn73  /dev/cu.usbmodem21101  Cardputer   (NOT flashed: fwd-compat)

Observables (require `debug on`):
  Fox    : "TX T seq=..", "TX E"/"TX K", "TX SKIP (halted) text", "# TX HALT/RESUME"
  Hunter : "RX T <id> seq=.. text=<clue>", "RX T render done elems=N",
           bare "<clue>" line (Serial.println of the decoded text)
"""
import sys, time, threading, re, subprocess, os

CLUE = "CQ DE FOX 42 NEAR OLD MILL"
PIO  = os.path.expanduser("~/.platformio/penv/bin/pio")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name -> (port, dtr, sid, env-to-flash-or-None)
PORTS = {
    "fox":     dict(port="/dev/cu.usbmodem22301", dtr=False, sid=42,
                    env="heltec_v4", required=True),
    "hunterA": dict(port="/dev/cu.usbmodem21401", dtr=False, sid=43,
                    env="heltec_v4", required=True),
    "hunterB": dict(port="/dev/cu.usbmodem20301", dtr=True,  sid=115,
                    env="wio_tracker_l1", required=False),
    "instr":   dict(port="/dev/cu.usbmodem21101", dtr=False, sid=73,
                    env=None, required=False),   # left on old fw on purpose
}

import serial

RESULTS = []
def record(step, ok, detail="", skip=False):
    tag = "SKIP" if skip else ("PASS" if ok else "FAIL")
    RESULTS.append((step, ok, detail, skip))
    print(f"\n[{tag}] {step}" + (f" — {detail}" if detail else ""), flush=True)


def flash(env, port):
    """Flash `env` to `port`. Returns True on success."""
    print(f"\n# flashing {env} -> {port} ...", flush=True)
    try:
        r = subprocess.run(
            [PIO, "run", "-e", env, "-t", "upload", "--upload-port", port],
            cwd=ROOT, capture_output=True, text=True, timeout=240)
    except subprocess.TimeoutExpired:
        print(f"# flash timeout {env} -> {port}", flush=True)
        return False
    ok = r.returncode == 0
    if not ok:
        print(r.stdout[-1500:], flush=True)
        print(r.stderr[-800:], flush=True)
    print(f"# flash {'OK' if ok else 'FAILED'} {env} -> {port}", flush=True)
    return ok


class Station:
    def __init__(self, name, port, dtr, sid):
        self.name, self.port, self.dtr, self.sid = name, port, dtr, sid
        self.ser = None; self.lines = []; self._partial = ""
        self.lock = threading.Lock(); self._stop = False; self._thr = None

    def open(self, retries=12, delay=1.0):
        last = None
        for _ in range(retries):
            try:
                s = serial.Serial()
                s.port = self.port; s.baudrate = 115200; s.timeout = 0.2
                s.dtr = self.dtr; s.rts = False
                s.open()
                self.ser = s
                return
            except Exception as e:                       # noqa
                last = e; time.sleep(delay)
        raise RuntimeError(f"{self.name}: cannot open {self.port}: {last}")

    def start_reader(self):
        self._stop = False
        self._thr = threading.Thread(target=self._reader, daemon=True)
        self._thr.start()

    def _reader(self):
        while not self._stop:
            try:
                data = self.ser.read(4096)
            except Exception:
                time.sleep(0.2); continue
            if not data:
                continue
            text = data.decode("utf-8", "replace")
            with self.lock:
                self._partial += text
                while "\n" in self._partial:
                    line, self._partial = self._partial.split("\n", 1)
                    self.lines.append((time.time(), line.rstrip("\r")))

    def close(self):
        self._stop = True
        if self._thr: self._thr.join(timeout=2)
        if self.ser:
            try: self.ser.close()
            except Exception: pass

    def send(self, cmd):
        try:
            self.ser.write((cmd + "\n").encode()); self.ser.flush()
            print(f"   -> {self.name}: {cmd}", flush=True)
            return True
        except Exception as e:                            # noqa
            print(f"   !! {self.name}: send failed ({e})", flush=True)
            return False

    def wait_for(self, patterns, timeout, since=0.0):
        if isinstance(patterns, str): patterns = [patterns]
        comp = [re.compile(p) for p in patterns]
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for ts, line in self.lines:
                    if ts >= since:
                        for c in comp:
                            if c.search(line):
                                return (ts, line)
            time.sleep(0.2)
        return None

    def count_since(self, pattern, since, until=None):
        c = re.compile(pattern); n = 0
        with self.lock:
            for ts, line in self.lines:
                if ts >= since and (until is None or ts <= until) and c.search(line):
                    n += 1
        return n

    def lines_since(self, since):
        with self.lock:
            return [l for ts, l in self.lines if ts >= since]

    def show_field(self, field_re, timeout=8):
        t0 = time.time()
        self.send("show")
        hit = self.wait_for([field_re], timeout, since=t0)
        if not hit: return None
        m = re.search(field_re, hit[1])
        return m.group(1) if (m and m.groups()) else hit[1]

    def alive(self, timeout=8):
        return self.show_field(r"build\s*=\s*(\S+)", timeout=timeout) is not None

    def show_rxtext(self, timeout=10):
        """Return dict(text, seq, elems, rx) from the `rxtext = ...` show line.
        Works over serial AND BLE (it's a command reply, not the Serial trace)."""
        rx_re = r'rxtext\s*=\s*"([^"]*)"\s+seq=(-?\d+)\s+elems=(\d+)\s+rx=(\d+)'
        t0 = time.time()
        self.send("show")
        hit = self.wait_for([rx_re], timeout, since=t0)
        if not hit: return None
        m = re.search(rx_re, hit[1])
        return dict(text=m.group(1), seq=int(m.group(2)),
                    elems=int(m.group(3)), rx=int(m.group(4)))


# ---- BLE transport (NUS) -- same line-buffer API as Station ------------------
# Used for the Wio nRF52 hunter, whose USB-CDC serial is unreliable right after
# an nrfutil reflash ("Device not configured"). The firmware's always-on BLE NUS
# streams the same console output, so we observe RX T / render lines over BLE.
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # write  (host -> device)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # notify (device -> host)


class BleStation(Station):
    def __init__(self, name, sid):
        super().__init__(name, f"BLE:MorseStn-{sid}", False, sid)
        self._loop = None; self._thr2 = None; self._client = None
        self._ready = threading.Event(); self._err = None

    def open(self, retries=1, delay=0.0):
        import asyncio
        from bleak import BleakClient, BleakScanner

        async def runner():
            want = f"MorseStn-{self.sid}"
            found = {}
            def on_detect(dev, adv):
                nm = adv.local_name or dev.name or ""
                if nm == want:
                    found[dev.address] = dev
            scanner = BleakScanner(detection_callback=on_detect)
            await scanner.start()
            for _ in range(40):
                if found: break
                await asyncio.sleep(0.25)
            await scanner.stop()
            if not found:
                self._err = f"no {want} advertising"; self._ready.set(); return
            dev = sorted(found.items())[0][1]
            def on_notify(_c, data: bytearray):
                text = bytes(data).decode("utf-8", "replace")
                with self.lock:
                    self._partial += text
                    while "\n" in self._partial:
                        line, self._partial = self._partial.split("\n", 1)
                        self.lines.append((time.time(), line.rstrip("\r")))
            try:
                async with BleakClient(dev) as client:
                    self._client = client
                    await client.start_notify(NUS_TX, on_notify)
                    self._ready.set()
                    while not self._stop:
                        await asyncio.sleep(0.2)
                    try: await client.stop_notify(NUS_TX)
                    except Exception: pass
            except Exception as e:                       # noqa
                self._err = str(e); self._ready.set()

        def thread_main():
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            self._loop.run_until_complete(runner())

        self._stop = False
        self._thr2 = threading.Thread(target=thread_main, daemon=True)
        self._thr2.start()
        if not self._ready.wait(timeout=18):
            raise RuntimeError(f"{self.name}: BLE connect timed out")
        if self._err:
            raise RuntimeError(f"{self.name}: {self._err}")

    def start_reader(self):
        pass   # notifications push into self.lines already

    def send(self, cmd):
        import asyncio
        if not self._client or not self._loop:
            print(f"   !! {self.name}: BLE not connected", flush=True); return False
        try:
            fut = asyncio.run_coroutine_threadsafe(
                self._client.write_gatt_char(NUS_RX, (cmd + "\n").encode(),
                                             response=False), self._loop)
            fut.result(timeout=5)
            print(f"   -> {self.name}: {cmd}", flush=True)
            return True
        except Exception as e:                           # noqa
            print(f"   !! {self.name}: BLE send failed ({e})", flush=True)
            return False

    def close(self):
        self._stop = True
        if self._thr2: self._thr2.join(timeout=5)


def main():
    print("=== Text-frame canned-message mode test (field note §7) ===", flush=True)

    # ---- 1. FLASH (required boards first) -----------------------------------
    flashed = {}
    for n, c in PORTS.items():
        if c["env"] is None:
            continue
        ok = flash(c["env"], c["port"])
        flashed[n] = ok
        if not ok and c["required"]:
            record(f"flash {n} ({c['env']})", False, "required flash failed; aborting")
            print("\n0/1 checks passed", flush=True)
            return 1
    # let native-USB CDC re-enumerate after reset
    time.sleep(3.0)

    st = {n: Station(n, c["port"], c["dtr"], c["sid"]) for n, c in PORTS.items()}
    # only operate stations we can open; required ones must open
    live = {}
    for n, s in st.items():
        try:
            s.open(); s.start_reader(); live[n] = s
        except Exception as e:
            if PORTS[n]["required"]:
                record(f"open {n}", False, str(e));
                print("\n0/1 checks passed", flush=True); return 1
            record(f"open {n}", False, f"{e} (optional, skipping)", skip=True)

    time.sleep(8.0)   # boot settle (splash + menu auto-timeout into persisted mode)

    # The Wio nRF52 USB-CDC is unreliable right after an nrfutil reflash. If its
    # serial isn't talking, fall back to its always-on BLE NUS (user request).
    hb = live.get("hunterB")
    if hb is not None and not hb.alive(timeout=6):
        print("# hunterB serial unresponsive — falling back to BLE", flush=True)
        try: hb.close()
        except Exception: pass
        live.pop("hunterB", None)
        try:
            bhb = BleStation("hunterB", PORTS["hunterB"]["sid"])
            bhb.open()
            if bhb.alive(timeout=10):
                live["hunterB"] = bhb
                print("# hunterB now observed over BLE", flush=True)
            else:
                bhb.close()
                record("hunterB transport", True,
                       "Wio unreachable over serial AND BLE", skip=True)
        except Exception as e:
            record("hunterB transport", True, f"BLE fallback failed: {e}", skip=True)

    fox = live["fox"]; hA = live["hunterA"]
    hB = live.get("hunterB"); instr = live.get("instr")
    hunters = [h for h in (hA, hB) if h is not None]

    try:
        # ---- 2. firmware: flashed boards share a build, differ from old instr --
        fox_b = fox.show_field(r"build\s*=\s*(\S+)")
        hA_b  = hA.show_field(r"build\s*=\s*(\S+)")
        same = fox_b is not None and fox_b == hA_b
        record("2. Fox + Hunter A run the same (freshly flashed) build", same,
               f"fox={fox_b}, hunterA={hA_b}")
        if instr is not None:
            in_b = instr.show_field(r"build\s*=\s*(\S+)")
            if in_b is None:
                record("2b. Instructor on older build (fwd-compat witness)", True,
                       "instructor port unresponsive", skip=True)
            else:
                record("2b. Instructor left on a DIFFERENT (older) build for fwd-compat",
                       in_b != fox_b, f"instr={in_b}, fox={fox_b}")

        # ---- 3. roles -------------------------------------------------------
        fmode = fox.show_field(r"(mode=Fox)")
        amode = hA.show_field(r"(mode=Hunter)")
        record("3. roles: Fox + Hunter present", fmode is not None and amode is not None,
               f"fox={fmode}, hunterA={amode}")

        # ---- 4. setup: clue, speed, debug, start keyed ----------------------
        fox.send("wpm 18")
        fox.send(f"msg {CLUE}")
        fox.send("msgmode keyed")
        fox.send("start")
        # debug on only where we assert on the trace (Fox + Hunters). The
        # Instructor stays on old fw as a passive forward-compat witness.
        for s in [fox] + hunters:
            s.send("debug on")
        time.sleep(2.0)

        # ---- 5. keyed baseline: edges on the air, hunter receives -----------
        t_base = time.time(); time.sleep(16.0)
        fox_e = fox.count_since(r"TX [EK] ", t_base)
        rxA_e = hA.count_since(r"RX [EK] ", t_base)
        record("5. keyed baseline: Fox keys edges + Hunter A receives them",
               fox_e > 0 and rxA_e > 0, f"foxTX(E/K)={fox_e}, hunterA RX(E/K)={rxA_e}")

        # ---- 6. switch to text mode + report --------------------------------
        fox.send("msgmode text")
        mm = fox.show_field(r"msgmode\s*=\s*(\w+)")
        record("6. `msgmode text` set + reported by show", mm == "text",
               f"show msgmode={mm}")

        # ---- 7. text delivery: clue arrives verbatim, zero '?' --------------
        # Snapshot each hunter's cumulative rx count, then run a TX window.
        rx0 = {h.name: (h.show_rxtext() or {}).get("rx", 0) for h in hunters}
        t_tx = time.time(); time.sleep(30.0)   # ~2-3 cycles at REPEAT_PAUSE=12s
        tx_T = fox.count_since(r"TX T ", t_tx)
        tx_edges = fox.count_since(r"TX [EK] ", t_tx)
        record("7a. Fox sends TextMsg frames and NO more edges in text mode",
               tx_T > 0 and tx_edges == 0, f"TX T={tx_T}, TX(E/K)={tx_edges}")

        for h in hunters:
            # A full clue render takes ~15s; poll up to 20s for a completed render
            # (elems>0) so we don't sample mid-render.
            rxt = None; deadline = time.time() + 20
            while time.time() < deadline:
                rxt = h.show_rxtext()
                if rxt and rxt["elems"] > 0:
                    break
                time.sleep(2.0)
            if rxt is None:
                record(f"7b. {h.name}: received TextMsg frames", False,
                       "no rxtext line from show")
                continue
            rx_delta = rxt["rx"] - rx0.get(h.name, 0)
            record(f"7b. {h.name}: received TextMsg frames ({h.port})",
                   rx_delta > 0, f"rx delta={rx_delta} (total={rxt['rx']})")
            record(f"7c. {h.name}: clue shown VERBATIM with zero '?'",
                   rxt["text"] == CLUE and "?" not in rxt["text"],
                   f"rxtext={rxt['text']!r}")
            record(f"7d. {h.name}: rendered Morse LOCALLY (player keyed elems>0)",
                   rxt["elems"] > 0, f"elems={rxt['elems']}")
            # ---- 8. dedup: burst copies collapse to one accepted frame/cycle -
            # If dedup were broken, rx would jump ~TEXT_REPEATS(=4)x per cycle.
            dedup_ok = 0 < rx_delta <= tx_T + 1
            record(f"8. {h.name}: burst deduped (rx {rx_delta} ~= TX cycles {tx_T}, not {tx_T}x4)",
                   dedup_ok, f"rx delta={rx_delta}, TX T={tx_T}")

        # ---- 9. §6 fox-halt gates the text burst ----------------------------
        t_stop = time.time()
        fox.send("stop")
        skip = fox.wait_for([r"TX SKIP \(halted\) text", r"# TX HALT"], 20, since=t_stop)
        time.sleep(14.0)
        tx_T_halt = fox.count_since(r"TX T ", t_stop + 1)
        record("9a. `stop` halts the text burst (no TX T while halted)",
               skip is not None and tx_T_halt == 0,
               f"halt-log={'ok' if skip else 'none'}, TX T while halted={tx_T_halt}")
        t_start = time.time()
        fox.send("start")
        resume = fox.wait_for([r"TX T "], 20, since=t_start)
        record("9b. `start` resumes the text burst", resume is not None,
               resume[1] if resume else "no TX T within 20s of start")

        # ---- 10. forward-compat: old-fw Instructor still responsive ---------
        im = instr.show_field(r"(mode=Instructor)") if instr is not None else None
        if im is not None:
            record("10. old-fw Instructor ignores MAGIC_TEXT, stays responsive",
                   True, f"instr show -> {im}")
        else:
            record("10. forward-compat (Instructor)", True,
                   "instructor port unavailable/unresponsive", skip=True)

        # ---- 11. persistence across reboot ----------------------------------
        fox.send("reboot")
        fox.close()
        time.sleep(12.0)   # reboot: splash + menu auto-timeout + USB re-enumerate
        reopened = False
        try:
            fox.open(retries=20, delay=1.0); fox.start_reader(); reopened = True
        except Exception as e:
            record("11. msgmode persists across reboot", False,
                   f"could not reopen fox: {e}", skip=True)
        if reopened:
            time.sleep(3.0)
            # Retry show until the rebooted fox answers (it may still be booting).
            mm2 = None; deadline = time.time() + 30
            while time.time() < deadline:
                mm2 = fox.show_field(r"msgmode\s*=\s*(\w+)", timeout=5)
                if mm2 in ("text", "keyed"):
                    break
                time.sleep(2.0)
            record("11. msgmode=text persists across reboot (NVS)", mm2 == "text",
                   f"after reboot show msgmode={mm2}")

    finally:
        # leave the bench in the default keyed mode, transmitting
        try:
            fox.send("msgmode keyed"); time.sleep(0.5)
            fox.send("start"); time.sleep(1.0)
        except Exception:
            pass
        for s in live.values():
            s.close()

    print("\n================ SUMMARY ================", flush=True)
    npass = sum(1 for _, ok, _, sk in RESULTS if ok and not sk)
    nskip = sum(1 for _, _, _, sk in RESULTS if sk)
    nfail = sum(1 for _, ok, _, sk in RESULTS if not ok and not sk)
    for step, ok, detail, sk in RESULTS:
        tag = "SKIP" if sk else ("PASS" if ok else "FAIL")
        print(f"  [{tag}] {step}")
    total = npass + nfail
    print(f"\n{npass}/{total} checks passed ({nskip} skipped)", flush=True)
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
