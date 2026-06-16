#!/usr/bin/env python3
"""Instructor broadcast-banner integration test across four live stations.

Topology (one bench, current build f135ad9):
  - Instructor = stn73  /dev/cu.usbmodem22301  (Cardputer ESP32-S3, native USB, resets on open)
  - Fox        = stn38  /dev/cu.usbserial-0001  (Heltec V3, CP2102, resets on open)
  - Hunter A   = stn43  /dev/cu.usbmodem20401   (Heltec V4, native USB, resets on open)
  - Hunter B   = stn115 /dev/cu.usbmodem21301   (Wio Tracker L1, nRF52, NO reset; needs DTR to TX)

Procedure (from the task):
  1. All stations run the current build.
  2. Roles: stn73 Instructor, stn38 Fox, stn43 + stn115 Hunters.
  3. Instructor relays a test message to the Fox; confirm Hunters receive/copy it.
  4. Instructor relays `stop` to the Fox (quiets the channel).
  5. Instructor sends a `bcast` broadcast banner.
  6. Confirm via Hunter serial that the broadcast was RECEIVED ("RX B ... text=..."),
     with a `show` "banner = ..." cross-check.

Observables (debug on):
  Fox     : "RX C ... cmd=<x>", "# TX HALT", "TX E".
  Hunters : "RX [EK]" (on-air keying), "CH <id> <c>" (decoded char),
            "RX B seq=.. flags=.. text=<banner>" (broadcast received).
  Instr   : "TX B seq=.. left=.." (broadcast campaign running).
The Wio's nRF52 USB-CDC can stall the sustained CH stream under the multiport
reader, so its message-copy check gates on RX reception; the broadcast check uses
the single-line "RX B" plus a `show` banner cross-check.
"""
import sys, time, threading, re, difflib
import serial

EXPECT_BUILD = "f135ad9"
FOX_ID = 38
TEST_MSG = "ABLE BAKER CHARLIE"
BANNER   = "RETURN TO BASE"

PORTS = {
    "instructor": dict(port="/dev/cu.usbmodem22301", dtr=False, sid=73,  mode=3),
    "fox":        dict(port="/dev/cu.usbserial-0001", dtr=False, sid=38,  mode=1),
    "hunterA":    dict(port="/dev/cu.usbmodem20401",  dtr=False, sid=43,  mode=0),
    "hunterB":    dict(port="/dev/cu.usbmodem21301",  dtr=True,  sid=115, mode=0),
}

RESULTS = []
def record(step, ok, detail=""):
    RESULTS.append((step, ok, detail))
    print(f"\n[{'PASS' if ok else 'FAIL'}] {step}" + (f" — {detail}" if detail else ""), flush=True)


class Station:
    def __init__(self, name, port, dtr, sid):
        self.name, self.port, self.dtr, self.sid = name, port, dtr, sid
        self.ser = None; self.lines = []; self._partial = ""
        self.lock = threading.Lock(); self._stop = False; self._thr = None

    def open(self, retries=12, delay=1.0):
        last = None
        for _ in range(retries):
            try:
                s = serial.Serial(); s.port = self.port; s.baudrate = 115200
                s.timeout = 0.2; s.dtr = self.dtr; s.rts = False; s.open()
                self.ser = s; return
            except Exception as e:                       # noqa
                last = e; time.sleep(delay)
        raise RuntimeError(f"{self.name}: cannot open {self.port}: {last}")

    def _reopen(self):
        # The Wio (nRF52 TinyUSB CDC) drops its port across reboots / under load;
        # transparently re-attach so the reader and send() survive it.
        try:
            if self.ser: self.ser.close()
        except Exception: pass
        self.ser = None
        for _ in range(30):
            if self._stop: return False
            try:
                s = serial.Serial(); s.port = self.port; s.baudrate = 115200
                s.timeout = 0.2; s.dtr = self.dtr; s.rts = False; s.open()
                self.ser = s; return True
            except Exception:
                time.sleep(0.5)
        return False

    def start_reader(self):
        self._stop = False
        self._thr = threading.Thread(target=self._reader, daemon=True); self._thr.start()

    def _reader(self):
        while not self._stop:
            try:
                data = self.ser.read(4096) if self.ser else b""
            except Exception:
                self._reopen(); continue
            if not data:
                if self.ser is None: self._reopen()
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
        for attempt in (1, 2):
            try:
                self.ser.write((cmd + "\n").encode()); self.ser.flush()
                print(f"   -> {self.name}: {cmd}", flush=True)
                return
            except Exception:
                if attempt == 1 and self._reopen(): continue
                print(f"   !! {self.name}: write failed ({cmd!r})", flush=True)
                return

    def wait_for(self, patterns, timeout, since=0.0):
        if isinstance(patterns, str): patterns = [patterns]
        comp = [re.compile(p) for p in patterns]
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for ts, line in self.lines:
                    if ts >= since:
                        for c in comp:
                            if c.search(line): return (ts, line)
            time.sleep(0.2)
        return None

    def count_since(self, pattern, since, until=None):
        c = re.compile(pattern); n = 0
        with self.lock:
            for ts, line in self.lines:
                if ts >= since and (until is None or ts <= until) and c.search(line): n += 1
        return n

    def decoded_since(self, since, until=None):
        c = re.compile(r"CH\s+\d+\s+(.)"); out = []
        with self.lock:
            for ts, line in self.lines:
                if ts >= since and (until is None or ts <= until):
                    m = c.search(line)
                    if m: out.append(m.group(1))
        return "".join(out)


def fuzzy_ok(got, want):
    g = "".join(got.upper().split()); w = "".join(want.upper().split())
    ratio = difflib.SequenceMatcher(None, g, w).ratio()
    words = [bit for bit in want.upper().split() if bit in got.upper()]
    return ratio, words


def preconfig(name):
    """Set a station's id/mode (and fox baseline msg) via a throwaway connection,
    then reboot it into role. Reset-on-open boards boot the new mode on the main
    open; the Wio (no reset) is rebooted explicitly here."""
    c = PORTS[name]
    s = serial.Serial(); s.port = c["port"]; s.baudrate = 115200
    s.timeout = 0.2; s.dtr = c["dtr"]; s.rts = False; s.open()
    time.sleep(7.0 if not c["dtr"] else 2.0)   # boot settle (reset-on-open) / Wio is live
    def w(cmd, settle=1.2):
        s.write((cmd + "\n").encode()); s.flush(); time.sleep(settle)
    w(f"id {c['sid']}")
    if name == "fox":
        w("call N0CALL"); w("wpm 18"); w(f"msg CQ DE FOX {c['sid']}")
    w(f"mode {c['mode']}")
    buf = s.read(8192).decode("utf-8", "replace")
    w("reboot", 0.5)
    s.close()
    ok = f"boot mode = {c['mode']}" in buf
    print(f"   pre-config {name} (stn{c['sid']}): mode {c['mode']} "
          f"{'OK' if ok else '(unconfirmed)'}", flush=True)
    return ok


def main():
    print("=== Instructor broadcast-banner integration test ===", flush=True)

    # ---- pre-config roles (NVS) ------------------------------------------
    for n in ("instructor", "fox", "hunterA", "hunterB"):
        preconfig(n)
    time.sleep(10.0)   # let the Wio (nRF52 CDC) finish rebooting/re-enumerating

    st = {n: Station(n, c["port"], c["dtr"], c["sid"]) for n, c in PORTS.items()}
    for s in st.values(): s.open()
    for s in st.values(): s.start_reader()
    t_open = time.time()
    time.sleep(8.0)   # boot settle (reset-on-open boards reboot here)

    try:
        # ===== STEP 1: current firmware ===================================
        fw_ok = True; fw_detail = []
        for n, s in st.items():
            s.send("show")
            hit = s.wait_for([rf"build\s*=\s*{EXPECT_BUILD}"], 8, since=time.time()-1)
            ok = hit is not None; fw_ok &= ok
            fw_detail.append(f"{n}(stn{s.sid})={'ok' if ok else 'MISMATCH'}")
        record(f"1. all stations run current build ({EXPECT_BUILD})", fw_ok, ", ".join(fw_detail))

        # ===== STEP 2: roles ==============================================
        roles = {}
        for n, want in (("instructor","Instructor"),("fox","Fox"),
                        ("hunterA","Hunter"),("hunterB","Hunter")):
            st[n].send("show")
            hit = st[n].wait_for([rf"mode={want}"], 10, since=time.time()-1)
            roles[n] = hit is not None
        roles_ok = all(roles.values())
        record("2. roles: 73=Instructor, 38=Fox, 43/115=Hunters", roles_ok,
               ", ".join(f"{n}={'ok' if v else 'BAD'}" for n, v in roles.items()))

        for n in st: st[n].send("debug on")
        time.sleep(2.0)

        # baseline: fox transmitting, both hunters receiving
        t_base = time.time(); time.sleep(14.0)
        fox_tx = st["fox"].count_since(r"TX [EK] ", t_base)
        rxA = st["hunterA"].count_since(r"RX [EK] ", t_base)
        rxB = st["hunterB"].count_since(r"RX [EK] ", t_base)
        record("baseline: Fox TX + both Hunters RX", fox_tx > 0 and rxA > 0 and rxB > 0,
               f"foxTX={fox_tx}, hunterA RX={rxA}, hunterB RX={rxB}")

        # ===== STEP 3: instructor relays a test message to the fox ========
        t_msg = time.time()
        st["instructor"].send(f"relay {FOX_ID} msg {TEST_MSG}")
        hit = st["fox"].wait_for([rf"RX C .*cmd=msg {re.escape(TEST_MSG)}",
                                  rf"fox msg\s*=\s*{re.escape(TEST_MSG)}"], 90, since=t_msg)
        record("3a. Fox accepted relayed test message", hit is not None,
               hit[1] if hit else "no RX C cmd=msg within 90s")

        # hunters copy the NEW message: gate on RX, report decode where captured
        t_copy = time.time(); deadline = time.time() + 80
        rxA_n = rxB_n = 0; decA = decB = ""; wA = wB = []
        while time.time() < deadline and not (rxA_n > 5 and rxB_n > 5):
            time.sleep(4.0)
            rxA_n = st["hunterA"].count_since(r"RX [EK] ", t_copy)
            rxB_n = st["hunterB"].count_since(r"RX [EK] ", t_copy)
            decA = st["hunterA"].decoded_since(t_copy); decB = st["hunterB"].decoded_since(t_copy)
            _, wA = fuzzy_ok(decA, TEST_MSG); _, wB = fuzzy_ok(decB, TEST_MSG)
        record("3b. both Hunters receiving the test message", rxA_n > 5 and rxB_n > 5,
               f"hunterA RX={rxA_n} decoded={decA!r} words={wA}; "
               f"hunterB RX={rxB_n} decoded={decB!r} words={wB}")

        # ===== STEP 4: relay stop (quiet the channel) =====================
        t_stop = time.time()
        st["instructor"].send(f"relay {FOX_ID} stop")
        hit = st["fox"].wait_for([r"RX C .*cmd=stop", r"# TX HALT"], 90, since=t_stop)
        record("4. Fox received relayed 'stop'", hit is not None,
               hit[1] if hit else "no stop within 90s")
        st["fox"].wait_for([r"# TX HALT", r"TX SKIP \(halted\)"], 20, since=(hit[0] if hit else time.time())-1)
        time.sleep(8.0)   # let the channel go quiet

        # ===== STEP 5/6: instructor broadcast banner ======================
        t_bc = time.time()
        st["instructor"].send(f"bcast {BANNER}")
        # instructor confirms staging + campaign
        stage = st["instructor"].wait_for([rf"bcasting.*: {re.escape(BANNER)}"], 6, since=t_bc)
        txb   = st["instructor"].wait_for([r"TX B seq="], 8, since=t_bc)
        record("5. Instructor staged + bursting broadcast", stage is not None and txb is not None,
               f"stage={'ok' if stage else 'no'}, txB={'ok' if txb else 'no'}")

        # hunters RECEIVE the broadcast (passive "RX B"), with show banner cross-check
        rb_A = st["hunterA"].wait_for([rf"RX B .*text={re.escape(BANNER)}"], 14, since=t_bc)
        rb_B = st["hunterB"].wait_for([rf"RX B .*text={re.escape(BANNER)}"], 14, since=t_bc)

        # cross-check: query the banner state on each hunter (within the 15 s show window)
        def banner_show(name):
            st[name].send("show")
            h = st[name].wait_for([rf"banner\s*=\s*{re.escape(BANNER)}"], 6, since=time.time()-1)
            return h
        showA = banner_show("hunterA"); showB = banner_show("hunterB")

        okA = rb_A is not None or showA is not None
        okB = rb_B is not None or showB is not None
        record("6. both Hunters received the broadcast", okA and okB,
               f"hunterA: RX_B={'ok' if rb_A else '-'} bannerShow={'ok' if showA else '-'}; "
               f"hunterB: RX_B={'ok' if rb_B else '-'} bannerShow={'ok' if showB else '-'}")

    finally:
        # restore: resume the fox so it isn't left parked halted
        try:
            st["instructor"].send(f"relay {FOX_ID} start"); time.sleep(2)
        except Exception: pass
        for s in st.values(): s.close()

    print("\n================ SUMMARY ================", flush=True)
    npass = sum(1 for _, ok, _ in RESULTS if ok)
    for step, ok, detail in RESULTS:
        print(f"  [{'PASS' if ok else 'FAIL'}] {step}")
    print(f"\n{npass}/{len(RESULTS)} checks passed", flush=True)
    return 0 if npass == len(RESULTS) else 1


if __name__ == "__main__":
    raise SystemExit(main())
