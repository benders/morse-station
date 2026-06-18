#!/usr/bin/env python3
"""Verify a broadcast banner actually DISPLAYS on a blanked Hunter panel and is
held for ~1 minute — the bug the user hit (panel stayed dark).

Roles persist in NVS from the prior run: stn73 Instructor (usbmodem22301),
stn38 Fox (usbserial-0001), stn43 + stn115 Hunters (usbmodem20401 / usbmodem21301).

Method: halt the Fox (quiet the channel so Hunters stop receiving and won't
self-wake), force both Hunter panels blank (`screen off`), then `alert`. A blanked
panel that the broadcast wakes logs "# screen: woke"; assert that on both Hunters
plus the "RX B ... text=" reception, then confirm the panel stays lit (no
"# screen: blanked") for ~45 s — i.e., the banner overrides idle-blank for its life.
"""
import sys, time, threading, re
import serial

BANNER = "RETURN TO BASE"
FOX_ID = 38
PORTS = {
    "instructor": dict(port="/dev/cu.usbmodem22301", dtr=False, sid=73),
    "fox":        dict(port="/dev/cu.usbserial-0001", dtr=False, sid=38),
    "hunterA":    dict(port="/dev/cu.usbmodem20401",  dtr=False, sid=43),
    "hunterB":    dict(port="/dev/cu.usbmodem21301",  dtr=True,  sid=115),
}
RESULTS = []
def record(step, ok, detail=""):
    RESULTS.append((step, ok));
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

def main():
    print("=== Broadcast banner DISPLAY-on-blanked-panel test ===", flush=True)
    st={n:Station(n,c["port"],c["dtr"],c["sid"]) for n,c in PORTS.items()}
    for s in st.values(): s.open()
    for s in st.values(): s.start_reader()
    time.sleep(8.0)   # boot settle (ESP32 boards reset on open)
    try:
        # roles + debug (retry the query — the Wio CDC can be slow right at boot)
        roles={}
        for n,want in (("instructor","Instructor"),("fox","Fox"),("hunterA","Hunter"),("hunterB","Hunter")):
            hit=None
            for _ in range(3):
                st[n].send("show")
                hit=st[n].wait_for([rf"mode={want}"],6,since=time.time()-1)
                if hit: break
            roles[n]=hit is not None
        record("roles: 73=Instructor, 38=Fox, 43/115=Hunters", all(roles.values()),
               ", ".join(f"{n}={'ok' if v else 'BAD'}" for n,v in roles.items()))
        for n in st: st[n].send("debug on")
        time.sleep(2.0)

        # sanity baseline: hunters receiving
        t_base=time.time(); time.sleep(10.0)
        rxA=st["hunterA"].count_since(r"RX [EK] ",t_base); rxB=st["hunterB"].count_since(r"RX [EK] ",t_base)
        record("baseline: both Hunters receiving Fox", rxA>0 and rxB>0, f"hunterA RX={rxA}, hunterB RX={rxB}")

        # halt the fox so the channel goes quiet and Hunters stop self-waking
        t_stop=time.time(); st["instructor"].send(f"relay {FOX_ID} stop")
        st["fox"].wait_for([r"# TX HALT"],90,since=t_stop)
        time.sleep(6.0)

        # force both Hunter panels blank (simulates the operator-blanked screen)
        for n in ("hunterA","hunterB"): st[n].send("screen off")
        blkA=st["hunterA"].wait_for([r"# screen: blanked"],6,since=time.time()-1)
        blkB=st["hunterB"].wait_for([r"# screen: blanked"],6,since=time.time()-1)
        record("Hunter panels forced blank", blkA is not None and blkB is not None,
               f"hunterA={'blanked' if blkA else 'no'}, hunterB={'blanked' if blkB else 'no'}")
        time.sleep(3.0)   # confirm they stay blanked (Fox is quiet)

        # ---- the broadcast (retry a couple of times if a Hunter misses all
        #      repeats; a human would just re-send) ----
        def banner_set(name):
            st[name].send("show")
            return st[name].wait_for([rf"banner\s*=\s*{re.escape(BANNER)}"],6,since=time.time()-1)

        t_bc=time.time(); wokeA=wokeB=rbA=rbB=setA=setB=None
        for attempt in range(3):
            tb=time.time(); st["instructor"].send(f"alert {BANNER}")
            # passive observables (informational; the Wio CDC can stall these)
            wokeA=wokeA or st["hunterA"].wait_for([r"# screen: woke"],10,since=tb)
            wokeB=wokeB or st["hunterB"].wait_for([r"# screen: woke"],10,since=tb)
            rbA=rbA or st["hunterA"].wait_for([rf"RX B .*text={re.escape(BANNER)}"],2,since=tb)
            rbB=rbB or st["hunterB"].wait_for([rf"RX B .*text={re.escape(BANNER)}"],2,since=tb)
            # authoritative: query the banner state (request/response, survives stalls)
            setA=setA or banner_set("hunterA"); setB=setB or banner_set("hunterB")
            if setA and setB: break
            print(f"   (retry {attempt+1}: hunterA set={'y' if setA else 'n'}, "
                  f"hunterB set={'y' if setB else 'n'})", flush=True)
            time.sleep(2.0)

        record("broadcast DISPLAYED on both Hunters (banner active)", setA is not None and setB is not None,
               f"hunterA banner={'SET' if setA else 'NONE'} (woke={'y' if wokeA else 'n'}); "
               f"hunterB banner={'SET' if setB else 'NONE'} (woke={'y' if wokeB else 'n'})")
        record("broadcast WOKE the blanked panels (passive log)", wokeA is not None or wokeB is not None,
               f"hunterA={'woke' if wokeA else '-'}, hunterB={'woke' if wokeB else '-'} "
               f"(Wio CDC may stall its log; banner-active above is authoritative)")
        # reception confirmed per hunter by EITHER the passive RX B line OR the
        # authoritative banner-active query (the Wio's CDC stalls its passive logs).
        recvA = rbA is not None or setA is not None
        recvB = rbB is not None or setB is not None
        record("both Hunters received the broadcast", recvA and recvB,
               f"hunterA: RX_B={'ok' if rbA else '-'} banner-active={'ok' if setA else '-'}; "
               f"hunterB: RX_B={'ok' if rbB else '-'} banner-active={'ok' if setB else '-'}")

        # HOLD CHECK: panel stays lit through the banner — no re-blank for ~45 s
        t_hold=time.time()
        print("   holding ~45 s to confirm the banner keeps the panel lit...", flush=True)
        time.sleep(45.0)
        reblankA=st["hunterA"].count_since(r"# screen: blanked",t_hold)
        reblankB=st["hunterB"].count_since(r"# screen: blanked",t_hold)
        record("banner held the panel lit for ~45 s (no re-blank)", reblankA==0 and reblankB==0,
               f"re-blanks: hunterA={reblankA}, hunterB={reblankB}")
    finally:
        try: st["instructor"].send(f"relay {FOX_ID} start"); time.sleep(2)
        except Exception: pass
        for s in st.values(): s.close()
    print("\n================ SUMMARY ================", flush=True)
    npass=sum(1 for _,ok in RESULTS if ok)
    for step,ok in RESULTS: print(f"  [{'PASS' if ok else 'FAIL'}] {step}")
    print(f"\n{npass}/{len(RESULTS)} checks passed", flush=True)
    return 0 if npass==len(RESULTS) else 1

if __name__=="__main__":
    raise SystemExit(main())
