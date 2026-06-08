#!/usr/bin/env python3
"""Unattended decode-test harness for edge-event keying (docs/edge-events.md, E6).

Drives the bench entirely over USB serial consoles -- no human keying or
listening:

  1. Resolve `--fox`/`--hunters` station ids to /dev/cu.usbmodem* ports the
     same way `scripts/devices.py --usb` does (live `show` over the runtime
     console; no hardcoded port names -- they re-enumerate).
  2. Provision the fox over its *boot setup console* (same flow as
     provision.py: reset -> "for setup console" -> 's' -> `setup>`): message,
     wpm, farns, keymode, `mode 1` (Fox persisted to NVS), `reboot`.
  3. Provision each hunter the same way: `mode 0` (Hunter), `reboot`.
  4. Wait for the boot sequence (setup window + splash + 5s menu auto-select)
     to land everyone in their run loop.
  5. Send `debug on` to each hunter over its *runtime* console (the same
     handler, non-blocking, reachable mid-loop -- see docs/edge-events.md
     "Unattended test instrumentation").
  6. Capture each hunter's serial concurrently (one thread per port) for
     `--duration` seconds, timestamping every line into `--log`.
  7. Parse `CH <sid> <c>` lines into a decoded string per hunter (a decoded
     space prints as `CH <sid> ` with a trailing space -- preserve it).
  8. Score each hunter by the best-matching run between its decoded text and
     the (repeatedly-sent) message -- see `score_decode()`. PASS if the ratio
     >= --pass-ratio.

This script ONLY talks to boards over serial; it never flashes firmware. It is
deliberately tolerant of a board that never reaches its prompt: that board is
marked FAIL with a clear reason and the run continues for the others.
"""
from __future__ import annotations

import argparse
import glob
import re
import sys
import threading
import time
from datetime import datetime

import serial  # provided by tools/blevenv or the PlatformIO venv

# ---------------------------------------------------------------------------
# Console framing constants (mirrors provision.py / devices.py).
# ---------------------------------------------------------------------------
SETUP_HINT = "for setup console"   # banner line that opens the ~2s setup window
SETUP_PROMPT = "setup>"            # REPL prompt once 's' is accepted
BOOT_SETTLE_S = 12.0               # setup window + splash + 5s menu auto-select

CH_RE = re.compile(r"^CH (\d+) (.)$")          # decoded-character line (space preserved)
CH_RE_EMPTY = re.compile(r"^CH (\d+) ?$")      # `CH <sid> ` with no trailing char captured
RX_RE = re.compile(r"^RX [EK] \d+ ")           # raw packet dump lines, for the human report


# ---------------------------------------------------------------------------
# Station -> port resolution (reuses devices.py's live runtime-console read).
# ---------------------------------------------------------------------------

def find_station_port(station_id: int, timeout: float = 4.0) -> str | None:
    """Probe every /dev/cu.usbmodem* for a board live-reporting `id=<station_id>`.

    Mirrors devices.py's usb_mode(): open each port, send `show` over the
    *runtime* console (non-disruptive -- a running station answers without a
    reset), and parse the `id=` field. We do this fresh each run because ports
    re-enumerate and a board may currently be mid-test-cycle from a prior run.
    """
    from devices import read_station_live  # local import: same dir as this script

    for port in sorted(glob.glob("/dev/cu.usbmodem*")):
        info = read_station_live(port, timeout=timeout)
        if info.get("id") == station_id:
            return port
    return None


def resolve_ports(station_ids: list[int]) -> dict[int, str]:
    """Resolve every requested station id to a port, or exit with a clear error."""
    print(f"# resolving stations {station_ids} -> /dev/cu.usbmodem* ports "
          f"(live `show` over runtime console)...")
    found: dict[int, str] = {}
    for sid in station_ids:
        port = find_station_port(sid)
        if port is None:
            sys.stderr.write(
                f"error: station {sid} not found on any /dev/cu.usbmodem* port "
                f"(is it powered, plugged in, and running -- not stuck at the "
                f"boot menu? try `scripts/devices.sh` to check)\n")
            sys.exit(2)
        found[sid] = port
        print(f"#   station {sid} -> {port}")
    return found


# ---------------------------------------------------------------------------
# Boot setup console: reset -> "for setup console" -> 's' -> setup> -> commands
# ---------------------------------------------------------------------------

def pulse_reset(s: serial.Serial) -> None:
    """RTS/DTR reset pulse -- the same best-effort sequence as provision.py."""
    try:
        s.setDTR(False); s.setRTS(True); time.sleep(0.1)
        s.setRTS(False); time.sleep(0.1)
        s.setDTR(True)
    except Exception:
        pass


def read_until(s: serial.Serial, needle: str, timeout: float, echo_prefix: str = "") -> bool:
    """Read lines until one contains `needle`, echoing them (for the log)."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = s.read(256)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            if echo_prefix and text:
                print(f"{echo_prefix}{text}")
            if needle in text:
                return True
        tail = buf.decode("utf-8", "replace")
        if needle in tail:           # prompt may arrive without a trailing \n
            return True
    return False


def send_setup_cmd(s: serial.Serial, cmd: str, label: str) -> None:
    print(f"  [{label}] > {cmd}")
    s.write((cmd + "\n").encode())
    s.flush()
    read_until(s, SETUP_PROMPT, timeout=3.0, echo_prefix=f"  [{label}] < ")


def provision_over_setup(port: str, label: str, commands: list[str],
                         reset_timeout: float = 14.0) -> bool:
    """Reset a board, enter its boot setup console, run `commands` in order,
    then `reboot`. Returns False (board marked FAIL by the caller) if the
    setup prompt never appears -- e.g. the board was already mid-boot-menu."""
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print(f"  [{label}] error: cannot open {port}: {e}")
        return False
    try:
        print(f"  [{label}] resetting {port}, waiting for setup prompt...")
        pulse_reset(s)
        if not read_until(s, SETUP_HINT, timeout=reset_timeout):
            print(f"  [{label}] error: never saw '{SETUP_HINT}' -- "
                  f"board may need a manual RST")
            return False
        s.write(b"s\n"); s.flush()
        if not read_until(s, SETUP_PROMPT, timeout=3.0):
            print(f"  [{label}] error: setup console did not open after 's'")
            return False
        for cmd in commands:
            send_setup_cmd(s, cmd, label)
        # `reboot` drops the console and restarts the board into boot_mode.
        print(f"  [{label}] > reboot")
        s.write(b"reboot\n"); s.flush()
        return True
    finally:
        try: s.close()
        except Exception: pass


# ---------------------------------------------------------------------------
# Runtime console: `debug on` (non-blocking; works mid-loop).
# ---------------------------------------------------------------------------

def send_runtime_debug_on(port: str, label: str) -> bool:
    """Open the port and send `debug on\\n` to the *runtime* console. The same
    `handle_setup_command` handler answers here (see docs/edge-events.md), so
    no prompt-waiting is needed -- just write the line."""
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print(f"  [{label}] error: cannot open {port} for `debug on`: {e}")
        return False
    try:
        s.write(b"debug on\n")
        s.flush()
        time.sleep(0.3)
        # Drain whatever came back so it doesn't bleed into the capture window.
        s.read(s.in_waiting or 1)
        print(f"  [{label}] sent `debug on` over runtime console ({port})")
        return True
    finally:
        try: s.close()
        except Exception: pass


# ---------------------------------------------------------------------------
# Concurrent capture: one thread per hunter port, timestamped lines -> log.
# ---------------------------------------------------------------------------

class Capture:
    """Captures one port's serial stream for `duration` seconds in a thread,
    timestamping each line and appending it to the shared log file (under a
    lock) plus this object's own `lines` list for per-hunter parsing."""

    def __init__(self, station_id: int, port: str, label: str):
        self.station_id = station_id
        self.port = port
        self.label = label
        self.lines: list[str] = []
        self.error: str | None = None
        self._thread: threading.Thread | None = None

    def start(self, duration: float, log_lock: threading.Lock, log_fh) -> None:
        self._thread = threading.Thread(
            target=self._run, args=(duration, log_lock, log_fh), daemon=True)
        self._thread.start()

    def join(self) -> None:
        if self._thread:
            self._thread.join()

    def _run(self, duration: float, log_lock: threading.Lock, log_fh) -> None:
        try:
            s = serial.Serial(self.port, 115200, timeout=0.2)
        except Exception as e:
            self.error = f"cannot open {self.port}: {e}"
            return
        try:
            s.reset_input_buffer()
            deadline = time.time() + duration
            while time.time() < deadline:
                try:
                    line = s.readline()
                except (serial.SerialException, OSError):
                    time.sleep(0.2)
                    continue
                if not line:
                    continue
                text = line.decode("utf-8", "replace").rstrip("\r\n")
                if not text:
                    continue
                ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                rec = f"[{ts}] [{self.label} {self.port}] {text}"
                self.lines.append(text)
                with log_lock:
                    log_fh.write(rec + "\n")
                    log_fh.flush()
        finally:
            try: s.close()
            except Exception: pass


# ---------------------------------------------------------------------------
# Decode parsing + scoring.
# ---------------------------------------------------------------------------

def decode_from_lines(lines: list[str]) -> str:
    """Reassemble `CH <sid> <c>` lines into a string, preserving spaces.

    A decoded space prints as `CH <sid> ` (trailing space, possibly with the
    character swallowed by line-splitting); both regexes below cope with that.
    """
    out = []
    for text in lines:
        m = CH_RE.match(text)
        if m:
            out.append(m.group(2))
            continue
        m = CH_RE_EMPTY.match(text)
        if m:
            out.append(" ")
    return "".join(out)


def longest_common_substring(a: str, b: str) -> int:
    """Classic DP longest-common-substring length. Both inputs are short
    (decoded text caps at a handful of repeats of `--msg`; the repeated
    expected string is bounded below), so the O(len(a)*len(b)) cost is fine."""
    if not a or not b:
        return 0
    prev = [0] * (len(b) + 1)
    best = 0
    for i in range(1, len(a) + 1):
        cur = [0] * (len(b) + 1)
        for j in range(1, len(b) + 1):
            if a[i - 1] == b[j - 1]:
                cur[j] = prev[j - 1] + 1
                best = max(best, cur[j])
        prev = cur
    return best


def score_decode(decoded: str, msg: str) -> float:
    """Best-overlap ratio between `decoded` and the message the fox repeats.

    The fox loops `msg` continuously and a hunter may join mid-message, so
    exact equality is the wrong test. Instead: build a long repeated/rotated
    version of the expected message (covering any join phase + wrap), find the
    longest common substring against the decoded text, and normalize by the
    length of the (single) message -- a ratio of 1.0 means the hunter decoded
    at least one full message cleanly; partial credit for partial decodes.
    """
    msg = msg.strip()
    if not msg or not decoded.strip():
        return 0.0
    # Repeat enough times to cover any join phase + at least 2 full cycles,
    # with a separating space the way loop_fox re-sends the message.
    reps = max(3, (len(decoded) // max(len(msg), 1)) + 2)
    expected = (msg + " ") * reps
    lcs = longest_common_substring(decoded, expected)
    return lcs / float(len(msg))


# ---------------------------------------------------------------------------
# Main flow.
# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(
        description="Unattended decode-test harness for edge-event keying "
                    "(docs/edge-events.md E6). Drives boards over serial only; "
                    "never flashes firmware.")
    p.add_argument("--fox", type=int, required=True, help="fox station id")
    p.add_argument("--hunters", required=True,
                   help="comma-separated hunter station ids, e.g. 115,42")
    p.add_argument("--keymode", choices=["compat", "edge"], default="edge",
                   help="fox keymode to provision (default: edge)")
    p.add_argument("--msg", default="PARIS PARIS PARIS",
                   help="fox message text (default: %(default)r)")
    p.add_argument("--wpm", type=int, default=13, help="overall keying speed")
    p.add_argument("--farns", type=int, default=None,
                   help="Farnsworth character speed (optional)")
    p.add_argument("--duration", type=float, default=40.0,
                   help="capture duration in seconds (default: 40)")
    p.add_argument("--log", default=None,
                   help="log file path (default: /tmp/edge_test-<ts>.log)")
    p.add_argument("--pass-ratio", type=float, default=0.6,
                   help="minimum overlap ratio to PASS a hunter (default: 0.6)")
    args = p.parse_args()

    try:
        hunter_ids = [int(x.strip()) for x in args.hunters.split(",") if x.strip()]
    except ValueError:
        p.error("--hunters must be a comma-separated list of integers")
    if not hunter_ids:
        p.error("--hunters must list at least one station id")
    if args.fox in hunter_ids:
        p.error("--fox station id must not also appear in --hunters")

    log_path = args.log or f"/tmp/edge_test-{int(time.time())}.log"
    print(f"# edge_test: fox={args.fox} hunters={hunter_ids} keymode={args.keymode} "
          f"msg={args.msg!r} wpm={args.wpm} farns={args.farns} "
          f"duration={args.duration}s log={log_path}")

    # --- 1. resolve all ports up front -------------------------------------
    ports = resolve_ports([args.fox] + hunter_ids)

    # --- 2. provision the fox over its setup console -----------------------
    print(f"\n# provisioning fox (station {args.fox}) @ {ports[args.fox]}")
    fox_cmds = [f"msg {args.msg}", f"wpm {args.wpm}"]
    if args.farns is not None:
        fox_cmds.append(f"farns {args.farns}")
    fox_cmds += [f"keymode {args.keymode}", "mode 1"]   # mode 1 = Fox (persisted)
    fox_ok = provision_over_setup(ports[args.fox], f"fox {args.fox}", fox_cmds)
    if not fox_ok:
        sys.stderr.write(f"error: failed to provision fox {args.fox}; aborting "
                         f"(no point capturing with no transmitter)\n")
        return 2

    # --- 3. provision each hunter over its setup console --------------------
    hunter_ok: dict[int, bool] = {}
    for sid in hunter_ids:
        print(f"\n# provisioning hunter (station {sid}) @ {ports[sid]}")
        hunter_ok[sid] = provision_over_setup(ports[sid], f"hunter {sid}",
                                               ["mode 0"])   # mode 0 = Hunter

    # --- 4. wait for the boot sequence to settle into the run loop ---------
    print(f"\n# waiting {BOOT_SETTLE_S:.0f}s for setup window + splash + "
          f"boot-menu auto-select on all boards...")
    time.sleep(BOOT_SETTLE_S)

    # --- 5. `debug on` over each hunter's runtime console -------------------
    for sid in hunter_ids:
        if hunter_ok[sid]:
            hunter_ok[sid] = send_runtime_debug_on(ports[sid], f"hunter {sid}")

    # --- 6. concurrent capture ----------------------------------------------
    print(f"\n# capturing {len(hunter_ids)} hunter(s) for {args.duration:.0f}s "
          f"-> {log_path}")
    log_fh = open(log_path, "w")
    log_lock = threading.Lock()
    captures: dict[int, Capture] = {}
    for sid in hunter_ids:
        cap = Capture(sid, ports[sid], f"hunter {sid}")
        captures[sid] = cap
        if hunter_ok[sid]:
            cap.start(args.duration, log_lock, log_fh)
    for sid in hunter_ids:
        if hunter_ok[sid]:
            captures[sid].join()
    log_fh.close()
    print(f"# capture complete; raw log at {log_path}")

    # --- 7/8. parse + score each hunter, print the report -------------------
    print(f"\n# === results (expected message: {args.msg!r}, "
          f"pass-ratio >= {args.pass_ratio}) ===")
    all_pass = True
    for sid in hunter_ids:
        cap = captures[sid]
        print(f"\n--- hunter {sid} ({ports[sid]}) ---")
        if not hunter_ok[sid]:
            print(f"  FAIL: provisioning/debug-enable failed -- "
                  f"{cap.error or 'see errors above'}")
            all_pass = False
            continue
        if cap.error:
            print(f"  FAIL: capture error -- {cap.error}")
            all_pass = False
            continue
        decoded = decode_from_lines(cap.lines)
        ratio = score_decode(decoded, args.msg)
        verdict = "PASS" if ratio >= args.pass_ratio else "FAIL"
        if verdict == "FAIL":
            all_pass = False
        print(f"  decoded : {decoded!r}")
        print(f"  expected: {args.msg!r} (repeated by the fox)")
        print(f"  ratio   : {ratio:.2f}  ->  {verdict}")
        rx_samples = [l for l in cap.lines if RX_RE.match(l)][:5]
        if rx_samples:
            print(f"  sample RX lines (proves edge vs compat packets received):")
            for l in rx_samples:
                print(f"    {l}")
        else:
            print(f"  (no RX E/RX K lines captured -- check `debug on` landed "
                  f"and the fox is actually transmitting)")

    summary = "PASS" if all_pass else "FAIL"
    print(f"\nSUMMARY: {summary} -- "
          f"{sum(1 for sid in hunter_ids if hunter_ok[sid])}/{len(hunter_ids)} "
          f"hunters reachable, log={log_path}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    # Make `from devices import read_station_live` resolve regardless of CWD.
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    sys.exit(main())
