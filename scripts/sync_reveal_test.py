#!/usr/bin/env python3
"""Per-element reveal bench test (field note §8, plan-text-sync-beacon S5).

S5 changes the hunter's progressive reveal so the dit/dah line gets ONE '.'/'-'
per key-down element as it sounds (the text line still shows a whole letter only
once complete). The dit/dah buffer is OLED-only, so the hunter emits a per-symbol
`TEL <.|->` debug line (mirroring the edge path's `EL`) which this test observes.

Asserts:
  1. the TEL symbol stream exactly equals the clue's Morse (dits/dahs in order);
  2. elements arrive PER-ELEMENT, not per-character — consecutive elements WITHIN
     a multi-element letter are separated in time by ~>= one dit (the old per-char
     reveal pushed a whole letter's symbols in a single instant).

Assumes boards are already flashed with the S5 build. Topology:
  - Fox      = stn42  /dev/cu.usbmodem201301  Heltec V4  (msgmode text)
  - Hunter   = stn38  /dev/cu.usbserial-4     Heltec V3  (resets on open)
Other foxes (stn73) are halted so the hunter locks onto the test fox.
"""
import sys, time, threading, re
import serial

WPM  = 18
CLUE = "CQ DE FOX 42 NEAR OLD MILL"

MORSE = {
    'A':".-",'B':"-...",'C':"-.-.",'D':"-..",'E':".",'F':"..-.",'G':"--.",
    'H':"....",'I':"..",'J':".---",'K':"-.-",'L':".-..",'M':"--",'N':"-.",
    'O':"---",'P':".--.",'Q':"--.-",'R':".-.",'S':"...",'T':"-",'U':"..-",
    'V':"...-",'W':".--",'X':"-..-",'Y':"-.--",'Z':"--..",
    '0':"-----",'1':".----",'2':"..---",'3':"...--",'4':"....-",'5':".....",
    '6':"-....",'7':"--...",'8':"---..",'9':"----.",
}
def expected_symbols(text):
    out = []
    for c in text.upper():
        if c == ' ':
            continue
        out.extend(list(MORSE.get(c, "")))
    return out
def char_lens(text):           # element count per non-space char, in order
    return [len(MORSE.get(c.upper(), "")) for c in text if c != ' ']

FOX = dict(port="/dev/cu.usbmodem201301", dtr=False)
HUN = dict(port="/dev/cu.usbserial-4",    dtr=False)
SILENCE = [dict(port="/dev/cu.usbmodem201201", dtr=False)]   # stn73 other fox

class Node:
    def __init__(self, cfg):
        self.lines = []; self._lock = threading.Lock(); self._run = True
        s = serial.Serial(); s.port=cfg["port"]; s.baudrate=115200; s.timeout=0.1
        s.dtr=cfg["dtr"]; s.rts=False; s.open(); self.ser=s
        threading.Thread(target=self._rd, daemon=True).start()
    def _rd(self):
        buf=b""
        while self._run:
            try: buf+=self.ser.read(256)
            except Exception: break
            while b"\n" in buf:
                ln,buf=buf.split(b"\n",1)
                t=ln.decode("u8","replace").strip("\r\n ")
                if t:
                    with self._lock: self.lines.append((time.time(), t))
    def send(self, c): self.ser.write((c+"\n").encode()); self.ser.flush(); time.sleep(0.3)
    def snap(self):
        with self._lock: return list(self.lines)
    def close(self):
        self._run=False; time.sleep(0.2)
        try: self.ser.close()
        except Exception: pass


def main():
    print("# opening ports...", flush=True)
    fox=Node(FOX); hun=Node(HUN)
    sil=[Node(c) for c in SILENCE]
    time.sleep(12)
    for s in sil: s.send("stop")

    print("# configuring...", flush=True)
    fox.send(f"wpm {WPM}"); fox.send(f"msg {CLUE}"); fox.send("msgmode text")
    fox.send("debug on"); fox.send("start")
    hun.send("debug on")

    # capture >=2 full render+pause cycles so we can isolate one COMPLETE render
    # bracketed by two `render done` markers (a clipped render would miscount).
    time.sleep(20)
    hun.lines.clear()
    print("# capturing 2+ cycles...", flush=True)
    time.sleep(80)

    fox.send(f"wpm {WPM}")
    for s in sil: s.send("start"); s.close()
    snap=hun.snap()
    fox.close(); hun.close()

    # ---- isolate ONE render: TEL lines bracketed by a fresh clue line --------
    # find a render-done to anchor a complete render, then take the TEL run before it
    tel=[(ts,t.split()[1]) for ts,t in snap if t.startswith("TEL ")]
    done=[ts for ts,t in snap if "render done" in t]
    ok_all=True
    def rec(step, ok, detail=""):
        nonlocal ok_all
        ok_all = ok_all and ok
        print(f"[{'PASS' if ok else 'FAIL'}] {step}" + (f" — {detail}" if detail else ""), flush=True)

    rec("0. hunter emitted TEL per-element trace", len(tel) > 10, f"{len(tel)} TEL lines")
    if len(done) < 2 or len(tel) < 10:
        print(f"\nFAILED: need >=2 complete renders (got {len(done)} render-done)", flush=True)
        return 1

    # isolate ONE complete render: all TEL between the last two `render done`
    # markers belong to the final render (preceded by a pause with no TEL).
    prev, d = done[-2], done[-1]
    run = [(ts,sym) for ts,sym in tel if prev < ts <= d]
    syms = [sym for _,sym in run]
    exp  = expected_symbols(CLUE)
    rec("1. TEL symbol stream equals the clue's Morse", syms == exp,
        f"got {len(syms)} syms, expected {len(exp)}"
        + ("" if syms==exp else f"\n      got={''.join(syms)}\n      exp={''.join(exp)}"))

    # per-element timing: walk char element-groups, check intra-char gaps
    dit_ms = 1200.0/WPM
    lens = char_lens(CLUE)
    i = 0; intra_gaps = []
    for L in lens:
        grp = run[i:i+L]; i += L
        for k in range(1, len(grp)):
            intra_gaps.append((grp[k][0]-grp[k-1][0])*1000.0)
    if intra_gaps:
        intra_gaps.sort()
        med = intra_gaps[len(intra_gaps)//2]
        mn  = min(intra_gaps)
        # per-char would push a letter's symbols in one instant (~0ms apart). Real
        # per-element gaps are >= dit (element + inter-element gap). Require median
        # well above 0 and above ~half a dit.
        rec("2. elements within a letter are time-separated (per-element, not per-char)",
            med >= dit_ms*0.5, f"median intra-letter gap={med:.0f}ms min={mn:.0f}ms (dit={dit_ms:.0f}ms)")
    else:
        rec("2. elements within a letter are time-separated", False, "no multi-element letters found")

    print(f"\n{'ALL PASS' if ok_all else 'SOME FAILED'}", flush=True)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
