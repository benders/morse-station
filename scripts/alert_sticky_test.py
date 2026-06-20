#!/usr/bin/env python3
"""Sticky-alert + fox-halt integration test (field note §6) across four stations.

Verifies the §6 behavior implemented in commit 693a4b1:
  - An instructor `alert <text>` latches on every RECEIVING station: the banner
    never auto-expires (survives past the 60 s BCAST_SHOW_MS) and a Fox halts TX.
  - `start` resumes the fox WITHOUT clearing the banner (explicit command wins).
  - `alert clear` releases the latch everywhere (banner gone, fox resumes).
  - `alert clear` is itself a resume trigger (no separate `start` needed).
  - A local button press does NOT dismiss a latched alert.
  - The INSTRUCTOR does not latch its own panel (must stay usable to send clear).

Topology (one bench; adjust ports/SIDs via scripts/devices.sh if re-enumerated):
  - Instructor = stn73  /dev/cu.usbmodem21101  (Cardputer ADV, native USB)
  - Fox        = stn42  /dev/cu.usbmodem22301  (Heltec V4, native USB)
  - Hunter A   = stn43  /dev/cu.usbmodem21401  (Heltec V4, native USB)
  - Hunter B   = stn115 /dev/cu.usbmodem20301  (Wio Tracker L1, nRF52; needs DTR)

Fully automated: every step asserts on passively-observed serial output. Only the
Instructor and (for the resume/btn checks) the Fox are written to. Observables:
  Fox     : "RX B seq=… sticky=1", "# TX HALT"/"TX SKIP (halted)", "TX E",
            "# TX RESUME".
  any     : "banner   = <text>  [ALERTED/LATCHED]" from `show` (the §6 observable).
  Hunters : "RX B …", banner shown.
Requires `debug on` (RX/TX lines are gated on it).
"""
import sys, time, threading, re
import serial

FOX_ID, INSTR_ID = 42, 43           # FOX_ID used in relay/alert addressing
ALERT_TEXT  = "RETURN TO BASE"
ALERT_TEXT2 = "STOP NOW"

PORTS = {
    "instructor": dict(port="/dev/cu.usbmodem21101", dtr=False, sid=73),
    "fox":        dict(port="/dev/cu.usbmodem22301", dtr=False, sid=42),
    "hunterA":    dict(port="/dev/cu.usbmodem21401", dtr=False, sid=43),
    "hunterB":    dict(port="/dev/cu.usbmodem20301", dtr=True,  sid=115),
}

LATCH = "[ALERTED/LATCHED]"
RESULTS = []
def record(step, ok, detail=""):
    RESULTS.append((step, ok, detail))
    print(f"\n[{'PASS' if ok else 'FAIL'}] {step}" + (f" — {detail}" if detail else ""), flush=True)


class Station:
    def __init__(self, name, port, dtr, sid):
        self.name, self.port, self.dtr, self.sid = name, port, dtr, sid
        self.ser = None
        self.lines = []
        self._partial = ""
        self.lock = threading.Lock()
        self._stop = False
        self._thr = None

    def open(self, retries=8, delay=1.0):
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
        self.ser.write((cmd + "\n").encode()); self.ser.flush()
        print(f"   -> {self.name}: {cmd}", flush=True)

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

    def show_banner(self, timeout=8):
        """Send `show` and return the `banner   = ...` line (or '')."""
        t0 = time.time()
        self.send("show")
        hit = self.wait_for([r"banner\s*=\s*"], timeout, since=t0)
        return hit[1] if hit else ""

    def wait_latch(self, want_latched, timeout=40, poll=4.0):
        """Poll `show` until the latch state matches `want_latched` (or timeout).
        A Fox only receives an over-the-air alert/clear in the tail of its
        message cycle, so reaching it can take up to ~one cycle (~15 s)."""
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            last = self.show_banner()
            if (LATCH in last) == want_latched:
                return True, last
            time.sleep(poll)
        return (LATCH in last) == want_latched, last

    def show_build(self, timeout=8):
        t0 = time.time()
        self.send("show")
        hit = self.wait_for([r"build\s*=\s*([0-9a-f]+)"], timeout, since=t0)
        if not hit: return None
        m = re.search(r"build\s*=\s*([0-9a-f]+)", hit[1])
        return m.group(1) if m else None


def main():
    print("=== Sticky-alert + fox-halt integration test (field note §6) ===", flush=True)

    st = {n: Station(n, c["port"], c["dtr"], c["sid"]) for n, c in PORTS.items()}
    for s in st.values():
        s.open()
    for s in st.values():
        s.start_reader()

    t_open = time.time()
    time.sleep(8.0)   # boot settle (ESP32/Cardputer reset on open)

    try:
        # ===== STEP 1: firmware consistent across the fleet ================
        builds = {}
        for n, s in st.items():
            builds[n] = s.show_build()
        uniq = set(b for b in builds.values() if b)
        fw_ok = len(uniq) == 1 and all(builds.values())
        record("1. all stations run the same firmware build", fw_ok,
               ", ".join(f"{n}={builds[n]}" for n in st))

        # ===== STEP 2: roles ==============================================
        st["instructor"].send("show")
        instr = st["instructor"].wait_for([r"mode=Instructor"], 10, since=time.time()-1)
        st["fox"].send("show")
        foxm = st["fox"].wait_for([r"mode=Fox"], 10, since=time.time()-1)
        roles_ok = instr is not None and foxm is not None
        record("2. roles: instructor + fox present", roles_ok,
               f"instr={'Instructor' if instr else '??'}, fox={'Fox' if foxm else '??'}")

        # speed the fox up so its RX windows are frequent; debug on everywhere.
        st["fox"].send("wpm 18")
        st["fox"].send(f"msg CQ DE FOX {FOX_ID}")
        for n in st:
            st[n].send("debug on")
        time.sleep(2.0)

        # baseline: fox transmitting + hunters receiving
        t_base = time.time()
        time.sleep(12.0)
        fox_tx = st["fox"].count_since(r"TX E ", t_base)
        rxA = st["hunterA"].count_since(r"RX [EK] ", t_base)
        rxB = st["hunterB"].count_since(r"RX [EK] ", t_base)
        base_ok = fox_tx > 0 and rxA > 0 and rxB > 0
        record("baseline: Fox TX + both Hunters RX", base_ok,
               f"foxTXe={fox_tx}, hunterA RX={rxA}, hunterB RX={rxB}")

        # ===== STEP 4: send sticky alert ==================================
        t_alert = time.time()
        st["instructor"].send(f"alert {ALERT_TEXT}")
        foxrx = st["fox"].wait_for([r"RX B .*sticky=1"], 90, since=t_alert)
        record("4a. Fox received sticky alert", foxrx is not None,
               foxrx[1] if foxrx else "no 'RX B sticky=1' within 90s")
        t_foxrx = foxrx[0] if foxrx else time.time()
        rxBnA = st["hunterA"].wait_for([r"RX B "], 30, since=t_alert)
        rxBnB = st["hunterB"].wait_for([r"RX B "], 30, since=t_alert)
        record("4b. both Hunters received the alert", rxBnA is not None and rxBnB is not None,
               f"hunterA={'RX B' if rxBnA else 'none'}, hunterB={'RX B' if rxBnB else 'none'}")
        # instructor must NOT latch its own panel
        ib = st["instructor"].show_banner()
        record("4c. Instructor does NOT latch its own panel", LATCH not in ib,
               f"instructor banner line: {ib!r}")

        # ===== STEP 5: fox halts ==========================================
        halt = st["fox"].wait_for([r"# TX HALT", r"TX SKIP \(halted\)"], 20, since=t_foxrx-1)
        record("5a. Fox halted on alert", halt is not None,
               halt[1] if halt else "no halt log within 20s")
        t_q = time.time(); time.sleep(14.0)
        fox_tx_after = st["fox"].count_since(r"TX E ", t_q)
        record("5b. Fox emits no keying while halted", fox_tx_after == 0,
               f"foxTXe in 14s halt window = {fox_tx_after}")

        # ===== STEP 6: banner is sticky (survives past 60s BCAST_SHOW_MS) ==
        # We've already waited ~26s+ since the alert; wait out the rest past 70s.
        elapsed = time.time() - t_foxrx
        if elapsed < 75:
            time.sleep(75 - elapsed)
        foxb = st["fox"].show_banner()
        hAb  = st["hunterA"].show_banner()
        hBb  = st["hunterB"].show_banner()
        sticky_ok = (LATCH in foxb) and (LATCH in hAb) and (LATCH in hBb)
        record("6a. banner still latched on Fox+Hunters after >70s", sticky_ok,
               f"fox={LATCH in foxb}, hunterA={LATCH in hAb}, hunterB={LATCH in hBb}")
        # fox stayed halted across the whole wait
        fox_tx_long = st["fox"].count_since(r"TX E ", t_foxrx)
        skip_long = st["fox"].count_since(r"TX SKIP \(halted\)", t_foxrx)
        record("6b. Fox stayed halted across the latch (0 TX, ≥1 SKIP)",
               fox_tx_long == 0 and skip_long >= 1,
               f"foxTXe since alert={fox_tx_long}, TX SKIP={skip_long}")
        # instructor's own transient banner has expired by now (no latch)
        ib2 = st["instructor"].show_banner()
        record("6c. Instructor banner expired (still no latch)", LATCH not in ib2,
               f"instructor banner line: {ib2!r}")

        # ===== STEP 10 (before start): local press does not dismiss latch ==
        # Inject a PRG press on the Fox via the `btn` console hook, then confirm
        # the alert is still latched (a sticky alert is not locally dismissable).
        st["fox"].send("btn")
        time.sleep(1.0)
        foxb_btn = st["fox"].show_banner()
        record("10. local press does not dismiss a latched alert", LATCH in foxb_btn,
               f"fox banner after btn: {foxb_btn!r}")

        # ===== STEP 7: `start` resumes TX WITHOUT clearing the banner ======
        t_start = time.time()
        st["instructor"].send(f"relay {FOX_ID} start")
        resume = st["fox"].wait_for([r"# TX RESUME", r"TX E "], 40, since=t_start)
        record("7a. Fox resumed TX on `start`", resume is not None,
               resume[1] if resume else "no resume within 40s")
        time.sleep(2.0)
        foxb3 = st["fox"].show_banner()
        record("7b. banner still latched after `start` (start ≠ clear)", LATCH in foxb3,
               f"fox banner after start: {foxb3!r}")

        # ===== STEP 8: `alert clear` releases everywhere ==================
        # Hunters (always in RX) clear at once; the Fox clears on its next RX
        # window — up to ~one message cycle later — so poll it for ~40 s.
        st["instructor"].send("alert clear")
        time.sleep(5.0)
        hAb4 = st["hunterA"].show_banner()
        hBb4 = st["hunterB"].show_banner()
        foxcl, foxb4 = st["fox"].wait_latch(want_latched=False, timeout=40)
        clear_ok = foxcl and (LATCH not in hAb4) and (LATCH not in hBb4)
        record("8. `alert clear` released the latch on Fox+Hunters", clear_ok,
               f"fox={foxcl}, hunterA={LATCH not in hAb4}, hunterB={LATCH not in hBb4}")

        # ===== STEP 9: re-alert, then `alert clear` ALONE resumes the fox ==
        t_a2 = time.time()
        st["instructor"].send(f"alert {ALERT_TEXT2}")
        foxrx2 = st["fox"].wait_for([r"RX B .*sticky=1"], 90, since=t_a2)
        halt2 = st["fox"].wait_for([r"# TX HALT", r"TX SKIP \(halted\)"], 20,
                                   since=(foxrx2[0] if foxrx2 else time.time())-1)
        record("9a. re-alert latched + halted the Fox again",
               foxrx2 is not None and halt2 is not None,
               f"rx={'ok' if foxrx2 else 'no'}, halt={'ok' if halt2 else 'no'}")
        # clear WITHOUT a separate start; fox must resume on the clear alone
        t_clear2 = time.time()
        st["instructor"].send("alert clear")
        resume2 = st["fox"].wait_for([r"# TX RESUME", r"TX E "], 40, since=t_clear2)
        record("9b. `alert clear` alone resumed the Fox (clear is a resume trigger)",
               resume2 is not None,
               resume2[1] if resume2 else "fox did not resume on clear within 40s")

    finally:
        # leave the bench un-alerted and transmitting
        try:
            st["instructor"].send("alert clear")
            time.sleep(1)
            st["instructor"].send(f"relay {FOX_ID} start")
            time.sleep(2)
        except Exception:
            pass
        for s in st.values():
            s.close()

    print("\n================ SUMMARY ================", flush=True)
    npass = sum(1 for _, ok, _ in RESULTS if ok)
    for step, ok, detail in RESULTS:
        print(f"  [{'PASS' if ok else 'FAIL'}] {step}")
    print(f"\n{npass}/{len(RESULTS)} checks passed", flush=True)
    return 0 if npass == len(RESULTS) else 1


if __name__ == "__main__":
    raise SystemExit(main())
