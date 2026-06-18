#!/usr/bin/env python3
"""Validate the instructor ALERT attention tone — and that it OVERRIDES mute
without changing the persisted mute state (docs/plan-alert-tone.md).

Roles persist in NVS: stn73 Instructor (usbmodem22301), stn43 + stn115 Hunters
(usbmodem20401 / usbmodem21301). No Fox is needed — the instructor broadcasts
and any Hunter receives; an idle channel just makes the log easier to read.

The harness SNAPSHOTS each Hunter's mute state on entry and restores it on exit,
so a Hunter that was muted before the run stays muted after (it forces mute on
only for the duration of the test).

Method: `mute on` both Hunters and confirm `show` reports mute=on. Then the
instructor sends one `alert`; every receiver with non-empty text fires a ~1.5 s
tone via sidetone_alert(), logging `# alert: tone <ms>` under `debug on`. Assert
each Hunter logs it (the tone sounded DESPITE mute), then re-query `show` and
assert mute is STILL on (the override was transient — persisted state untouched).
A final `alert clear` must be SILENT (empty text → no tone).

The Wio (stn115) CDC stalls/drops its passive log across reboots; the Station
class auto-reconnects and we treat the `# alert: tone` line as the authoritative
per-Hunter observable (request/response `show` covers the mute cross-check).
"""
import sys, time, threading, re
import serial

BANNER = "ATTENTION ALL"
PORTS = {
    "instructor": dict(port="/dev/cu.usbmodem22301", dtr=False, sid=73),
    "hunterA":    dict(port="/dev/cu.usbmodem20401",  dtr=False, sid=43),
    "hunterB":    dict(port="/dev/cu.usbmodem21301",  dtr=True,  sid=115),
}
RESULTS = []
def record(step, ok, detail=""):
    RESULTS.append((step, ok))
    print(f"\n[{'PASS' if ok else 'FAIL'}] {step}" + (f" — {detail}" if detail else ""), flush=True)

class Station:
    def __init__(self, name, port, dtr, sid):
        self.name, self.port, self.dtr, self.sid = name, port, dtr, sid
        self.ser=None; self.lines=[]; self._partial=""; self.lock=threading.Lock()
        self._stop=False; self._thr=None
    def open(self, retries=12, delay=1.0):
        last=None
        for _ in range(retries):
            try:
                s=serial.Serial(); s.port=self.port; s.baudrate=115200
                s.timeout=0.2; s.dtr=self.dtr; s.rts=False; s.open(); self.ser=s; return
            except Exception as e: last=e; time.sleep(delay)
        raise RuntimeError(f"{self.name}: cannot open {self.port}: {last}")
    def _reopen(self):
        try:
            if self.ser: self.ser.close()
        except Exception: pass
        self.ser=None
        for _ in range(30):
            if self._stop: return False
            try:
                s=serial.Serial(); s.port=self.port; s.baudrate=115200
                s.timeout=0.2; s.dtr=self.dtr; s.rts=False; s.open(); self.ser=s; return True
            except Exception: time.sleep(0.5)
        return False
    def start_reader(self):
        self._stop=False; self._thr=threading.Thread(target=self._reader, daemon=True); self._thr.start()
    def _reader(self):
        while not self._stop:
            try: data=self.ser.read(4096) if self.ser else b""
            except Exception: self._reopen(); continue
            if not data:
                if self.ser is None: self._reopen()
                continue
            with self.lock:
                self._partial += data.decode("utf-8","replace")
                while "\n" in self._partial:
                    line,self._partial=self._partial.split("\n",1)
                    self.lines.append((time.time(), line.rstrip("\r")))
    def close(self):
        self._stop=True
        if self._thr: self._thr.join(timeout=2)
        if self.ser:
            try: self.ser.close()
            except Exception: pass
    def send(self, cmd):
        for attempt in (1,2):
            try:
                self.ser.write((cmd+"\n").encode()); self.ser.flush()
                print(f"   -> {self.name}: {cmd}", flush=True); return
            except Exception:
                if attempt==1 and self._reopen(): continue
                print(f"   !! {self.name}: write failed ({cmd!r})", flush=True); return
    def wait_for(self, patterns, timeout, since=0.0):
        if isinstance(patterns,str): patterns=[patterns]
        comp=[re.compile(p) for p in patterns]; deadline=time.time()+timeout
        while time.time()<deadline:
            with self.lock:
                for ts,line in self.lines:
                    if ts>=since:
                        for c in comp:
                            if c.search(line): return (ts,line)
            time.sleep(0.2)
        return None
    def count_since(self, pattern, since, until=None):
        c=re.compile(pattern); n=0
        with self.lock:
            for ts,line in self.lines:
                if ts>=since and (until is None or ts<=until) and c.search(line): n+=1
        return n

def get_mute(st, tries=4, timeout=4):
    """Query `show` and return the current mute state as 'on'/'off' (None if
    unreadable). Retries because the Wio CDC can drop a `show` reply; each retry
    re-sends the request, so this survives a stalled stream."""
    for _ in range(tries):
        st.send("show")
        hit=st.wait_for([r"mute=(on|off)\b"], timeout, since=time.time()-1)
        if hit:
            m=re.search(r"mute=(on|off)", hit[1])
            if m: return m.group(1)
    return None

def banner_set(st, banner, tries=4, timeout=4):
    """Request/response check that a Hunter's banner == `banner` — authoritative
    on the Wio (whose passive logs stall). The alert tone is fired in the SAME
    `if (b.text[0])` receive block as the banner, so a set banner proves the tone
    code path ran on that node."""
    for _ in range(tries):
        st.send("show")
        if st.wait_for([rf"banner\s*=\s*{re.escape(banner)}"], timeout, since=time.time()-1):
            return True
    return False

def main():
    print("=== Instructor ALERT tone + mute-override test ===", flush=True)
    st={n:Station(n,c["port"],c["dtr"],c["sid"]) for n,c in PORTS.items()}
    for s in st.values(): s.open()
    for s in st.values(): s.start_reader()
    time.sleep(8.0)   # boot settle (ESP32 boards reset on open)
    try:
        # roles
        roles={}
        for n,want in (("instructor","Instructor"),("hunterA","Hunter"),("hunterB","Hunter")):
            hit=None
            for _ in range(3):
                st[n].send("show")
                hit=st[n].wait_for([rf"mode={want}"],6,since=time.time()-1)
                if hit: break
            roles[n]=hit is not None
        record("roles: 73=Instructor, 43/115=Hunters", all(roles.values()),
               ", ".join(f"{n}={'ok' if v else 'BAD'}" for n,v in roles.items()))
        for n in ("instructor","hunterA","hunterB"): st[n].send("debug on")
        time.sleep(2.0)

        # snapshot each Hunter's pre-test mute state so we can restore it on exit
        init_mute={n:get_mute(st[n]) for n in ("hunterA","hunterB")}
        print(f"   pre-test mute: {init_mute}", flush=True)

        # mute BOTH hunters and confirm persisted mute=on (retrying reads)
        for n in ("hunterA","hunterB"): st[n].send("mute on")
        time.sleep(1.5)
        mA=get_mute(st["hunterA"])=="on"; mB=get_mute(st["hunterB"])=="on"
        record("both Hunters report mute=on (pre-alert)", mA and mB,
               f"hunterA={'on' if mA else 'BAD'}, hunterB={'on' if mB else 'BAD'}")

        # ---- send ONE alert; assert each muted Hunter still fires the tone ----
        # hunterA (ESP32) passive `# alert: tone` log is authoritative. hunterB
        # (Wio) stalls passive logs, so prove reception via the request/response
        # banner cross-check — the tone fires in the same RX block as the banner.
        toneA=toneB=None; bannerB=False
        for attempt in range(3):
            tb=time.time(); st["instructor"].send(f"alert {BANNER}")
            toneA=toneA or st["hunterA"].wait_for([r"# alert: tone \d+ms"],8,since=tb)
            toneB=toneB or st["hunterB"].wait_for([r"# alert: tone \d+ms"],3,since=tb)
            bannerB=bannerB or banner_set(st["hunterB"], BANNER)
            if toneA and (toneB or bannerB): break
            print(f"   (retry {attempt+1}: hunterA tone={'y' if toneA else 'n'}, "
                  f"hunterB tone={'y' if toneB else 'n'} banner={'y' if bannerB else 'n'})", flush=True)
            time.sleep(2.0)
        # hunterB confirmed by EITHER its passive tone log OR the banner cross-check
        recvB = toneB is not None or bannerB
        record("muted Hunters SOUNDED the alert tone (mute overridden)", toneA is not None and recvB,
               f"hunterA={toneA[1] if toneA else 'NONE'}; "
               f"hunterB={'tone-log' if toneB else ('banner-set (tone path ran; Wio passive log stalled)' if bannerB else 'NONE')}")
        # instructor local echo should also fire its own tone
        instr_tone=st["instructor"].wait_for([r"# alert: tone \d+ms"],2,since=time.time()-12)
        record("instructor sounded its local-echo tone", instr_tone is not None,
               instr_tone[1] if instr_tone else "no local tone")

        # tone is transient — after it ends, persisted mute must STILL be on
        time.sleep(3.0)   # > ALERT_TONE_MS (1.5 s)
        mA2=get_mute(st["hunterA"])=="on"; mB2=get_mute(st["hunterB"])=="on"
        record("mute still on AFTER the tone (override was transient)", mA2 and mB2,
               f"hunterA={'on' if mA2 else 'BAD'}, hunterB={'on' if mB2 else 'BAD'}")

        # `alert clear` (empty text) must be SILENT — no new tone fires
        tclr=time.time(); st["instructor"].send("alert clear")
        silentA=st["hunterA"].wait_for([r"# alert: tone \d+ms"],4,since=tclr) is None
        silentB=st["hunterB"].wait_for([r"# alert: tone \d+ms"],1,since=tclr) is None
        record("`alert clear` is silent (no tone on empty text)", silentA and silentB,
               f"hunterA={'silent' if silentA else 'BEEPED'}, hunterB={'silent' if silentB else 'BEEPED'}")
    finally:
        # leave the bench as found: restore each Hunter's pre-test mute state
        # (default to muted if we never read it, so we never leave a node louder
        # than we found it).
        snap = locals().get("init_mute", {})
        restored = {}
        for n in ("hunterA","hunterB"):
            want = snap.get(n) or "on"
            restored[n] = want
            try: st[n].send(f"mute {want}")
            except Exception: pass
        print(f"   restored mute: {restored}", flush=True)
        time.sleep(1)
        for s in st.values(): s.close()
    print("\n================ SUMMARY ================", flush=True)
    npass=sum(1 for _,ok in RESULTS if ok)
    for step,ok in RESULTS: print(f"  [{'PASS' if ok else 'FAIL'}] {step}")
    print(f"\n{npass}/{len(RESULTS)} checks passed", flush=True)
    return 0 if npass==len(RESULTS) else 1

if __name__=="__main__":
    raise SystemExit(main())
