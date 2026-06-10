#!/usr/bin/env python3
"""Per-device carrier frequency-drift measurement via an rtl_tcp SDR.

The stations transmit GFSK at a nominal 905.000 MHz (see FREQ_MHZ in
src/radio.cpp). Each board's SX1262 TCXO has its own trim error, so every
transmitter's true carrier sits a few hundred Hz to a few kHz off nominal. This
script measures that offset with a cheap RTL-SDR dongle served over rtl_tcp, and
builds a per-station drift table.

It pairs with the firmware `txcw` console command, which keys an UNMODULATED
carrier (SX1262 SetTxContinuousWave) at exactly 905.000 MHz — a single spectral
peak the SDR can locate precisely (no GFSK demod needed). The script can drive
`txcw` for you over USB serial, either directly to a connected station or via
`relay <id> txcw` through a connected Instructor station.

  rtl_tcp side (separate terminal, or any host the dongle is plugged into):
      rtl_tcp -a 0.0.0.0            # listens on :1234

  Measure whatever carrier is on air right now (you key `txcw` yourself):
      ~/.platformio/penv/bin/python scripts/sdr_drift.py --once

  Drive one USB-connected station and measure it:
      ... scripts/sdr_drift.py --station 42 --secs 6

  Sweep several USB-connected stations into a table, 3 reads each:
      ... scripts/sdr_drift.py --stations 42,43,115 --repeat 3

  Measure distant stations through a USB-connected Instructor (id 7):
      ... scripts/sdr_drift.py --via 7 --stations 42,43 --secs 10

Accuracy notes:
  * The dongle's own reference sets the ABSOLUTE error. A 0.5 ppm TCXO dongle
    (e.g. Nooelec SMArt) is ~450 Hz at 905 MHz; correct it with --ppm or, better,
    pick one station as --ref so every other number is RELATIVE to it (the
    dongle's offset is common-mode and cancels — valid even on a junk dongle).
  * The frequency resolution here is sub-Hz; the limit is the dongle, not the FFT.
"""
from __future__ import annotations

import argparse
import glob
import re
import socket
import struct
import sys
import time

import numpy as np
import serial  # PlatformIO venv

# --- station console (USB serial) --------------------------------------------
BAUD = 115200
NOMINAL_HZ = 905_000_000          # FREQ_MHZ in src/radio.cpp (×1e6)


def ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def cmd(s: serial.Serial, line: str, read_t: float = 1.2) -> str:
    """Send one console line, return whatever comes back within read_t."""
    out = []
    try:
        s.reset_input_buffer()
        s.write((line + "\n").encode())
        s.flush()
        end = time.time() + read_t
        while time.time() < end:
            ln = s.readline()
            if ln:
                out.append(ln.decode("utf-8", "replace").rstrip())
    except (serial.SerialException, OSError):
        pass
    return "\n".join(out)


def identify() -> dict[int, str]:
    """Map station id -> USB port via the reset-free `show` command."""
    m: dict[int, str] = {}
    for p in ports():
        try:
            s = serial.Serial(p, BAUD, timeout=0.2)
        except Exception:
            continue
        try:
            time.sleep(0.4)
            mo = re.search(r"\bid=(\d+)", cmd(s, "show", 1.5))
            if mo:
                m[int(mo.group(1))] = p
        finally:
            s.close()
    return m


# --- rtl_tcp client ----------------------------------------------------------
class RtlTcp:
    """Minimal rtl_tcp client: set tuning, stream 8-bit I/Q.

    Wire protocol (see osmocom rtl_tcp.c): on connect the server sends a 12-byte
    header ("RTL0", tuner_type u32, gain_count u32); commands are 5 bytes,
    [id u8][param u32 big-endian]. Samples stream as interleaved unsigned bytes
    I,Q,I,Q,... centred on 127.4.
    """

    CMD_FREQ, CMD_RATE, CMD_GAIN_MODE = 0x01, 0x02, 0x03
    CMD_GAIN, CMD_PPM, CMD_AGC = 0x04, 0x05, 0x08

    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=5.0)
        hdr = self._recv_exact(12)
        magic, self.tuner_type, self.gain_count = struct.unpack(">4sII", hdr)
        if magic != b"RTL0":
            raise RuntimeError(f"not an rtl_tcp server (magic {magic!r})")

    def _send(self, cmd_id: int, param: int) -> None:
        # param may be signed (ppm); pack the low 32 bits.
        self.sock.sendall(struct.pack(">BI", cmd_id, param & 0xFFFFFFFF))

    def set_rate(self, hz: int) -> None:   self._send(self.CMD_RATE, hz)
    def set_freq(self, hz: int) -> None:   self._send(self.CMD_FREQ, hz)
    def set_ppm(self, ppm: int) -> None:   self._send(self.CMD_PPM, ppm)

    def set_gain_auto(self) -> None:
        self._send(self.CMD_GAIN_MODE, 0)   # tuner AGC
        self._send(self.CMD_AGC, 0)         # RTL2832 digital AGC off

    def set_gain(self, tenths_db: int) -> None:
        self._send(self.CMD_GAIN_MODE, 1)
        self._send(self.CMD_GAIN, tenths_db)

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("rtl_tcp closed")
            buf.extend(chunk)
        return bytes(buf)

    def read_iq(self, n_complex: int) -> np.ndarray:
        raw = np.frombuffer(self._recv_exact(2 * n_complex), dtype=np.uint8)
        iq = raw.astype(np.float32) - 127.4
        return iq[0::2] + 1j * iq[1::2]

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:
            pass


# --- carrier estimation ------------------------------------------------------
FS = 250_000          # valid RTL2832U rate (225001..300000), ±125 kHz of view
OFFSET_HZ = 50_000    # tune below nominal so the carrier dodges the DC spike
NFFT = 1 << 17        # ~1.9 Hz/bin at FS; frames are averaged for jitter
DC_GUARD_HZ = 3_000   # ignore the SDR's centre DC spike when peak-finding


def _peak_freq(frame: np.ndarray, fs: int) -> tuple[float, float]:
    """Return (carrier_offset_hz_from_centre, snr_db) for one I/Q frame."""
    win = np.hanning(len(frame))
    spec = np.fft.fftshift(np.fft.fft(frame * win))
    mag = np.abs(spec)
    freqs = np.fft.fftshift(np.fft.fftfreq(len(frame), 1.0 / fs))

    mag[np.abs(freqs) < DC_GUARD_HZ] = 0.0       # blank the DC spike
    k = int(np.argmax(mag))
    noise = np.median(mag[mag > 0]) or 1e-9
    snr_db = 20.0 * np.log10(mag[k] / noise)

    # Parabolic interpolation on the log-magnitude for sub-bin precision.
    if 0 < k < len(mag) - 1 and mag[k - 1] > 0 and mag[k + 1] > 0:
        a, b, c = np.log(mag[k - 1]), np.log(mag[k]), np.log(mag[k + 1])
        delta = 0.5 * (a - c) / (a - 2 * b + c)
    else:
        delta = 0.0
    bin_hz = fs / len(frame)
    return freqs[k] + delta * bin_hz, snr_db


def measure(rtl: RtlTcp, nominal_hz: int, capture_s: float,
            min_snr: float = 12.0) -> dict | None:
    """Capture for capture_s and return the median carrier across present frames.

    Returns {freq_hz, drift_hz, ppm, jitter_hz, snr_db, frames} or None if no
    carrier crossed min_snr (e.g. nothing keyed / relay never reached the fox).
    """
    centre = nominal_hz - OFFSET_HZ
    rtl.set_freq(centre)
    time.sleep(0.05)
    rtl.read_iq(FS // 4)            # flush retune transient

    n_frames = max(1, int(capture_s * FS / NFFT))
    freqs_hz, snrs = [], []
    for _ in range(n_frames):
        offs, snr = _peak_freq(rtl.read_iq(NFFT), FS)
        if snr >= min_snr:
            freqs_hz.append(centre + offs)
            snrs.append(snr)

    if not freqs_hz:
        return None
    arr = np.array(freqs_hz)
    freq = float(np.median(arr))
    drift = freq - nominal_hz
    return {
        "freq_hz": freq,
        "drift_hz": drift,
        "ppm": drift / nominal_hz * 1e6,
        "jitter_hz": float(np.std(arr)),
        "snr_db": float(np.median(snrs)),
        "frames": len(freqs_hz),
    }


# --- orchestration -----------------------------------------------------------
def key_and_measure(rtl: RtlTcp, console: serial.Serial | None, prefix: str,
                    secs: int, repeat: int) -> list[dict]:
    """Optionally key txcw over `console`, then measure `repeat` times."""
    results = []
    for _ in range(repeat):
        if console is not None:
            cmd(console, f"{prefix}txcw {secs}", 0.4)   # fire, don't wait
            time.sleep(0.6)                              # let the carrier come up
        # Capture nearly the whole keyed window; measure() only counts frames
        # with a real carrier, so a long window simply rides out relay latency.
        r = measure(rtl, NOMINAL_HZ, capture_s=max(secs - 1.0, 1.0) if console else 2.0)
        if r:
            results.append(r)
        if console is not None:
            # If we keyed remotely it auto-stops; for a direct/long key, stop now.
            cmd(console, f"{prefix}txcw off", 0.3)
            time.sleep(0.3)
    return results


def fmt_row(label: str, r: dict | None, ref_hz: float | None) -> str:
    if r is None:
        return f"  {label:>10}   {'— no carrier —':>34}"
    rel = ""
    if ref_hz is not None:
        rel = f"  rel={r['freq_hz'] - ref_hz:+8.0f} Hz"
    return (f"  {label:>10}   {r['freq_hz']/1e6:11.6f} MHz"
            f"  drift={r['drift_hz']:+8.0f} Hz ({r['ppm']:+6.3f} ppm)"
            f"  jit={r['jitter_hz']:5.0f}  snr={r['snr_db']:4.0f}dB"
            f"  n={r['frames']}{rel}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1234, help="rtl_tcp port")
    ap.add_argument("--ppm", type=int, default=0, help="dongle freq correction (ppm)")
    ap.add_argument("--gain", type=float, default=None,
                    help="manual tuner gain in dB (default: auto)")
    ap.add_argument("--once", action="store_true",
                    help="measure the carrier on air now; don't drive any station")
    ap.add_argument("--station", type=int, help="drive+measure one station id (USB)")
    ap.add_argument("--stations", help="comma list of station ids to sweep (USB)")
    ap.add_argument("--via", type=int, metavar="INSTRUCTOR_ID",
                    help="relay txcw through this USB-connected Instructor station")
    ap.add_argument("--secs", type=int, default=6, help="txcw key duration")
    ap.add_argument("--repeat", type=int, default=1, help="reads per station")
    ap.add_argument("--ref", type=int, help="station id to treat as reference (relative table)")
    args = ap.parse_args()

    try:
        rtl = RtlTcp(args.host, args.port)
    except Exception as e:
        print(f"! cannot reach rtl_tcp at {args.host}:{args.port}: {e}", file=sys.stderr)
        print("  start it with:  rtl_tcp -a 0.0.0.0", file=sys.stderr)
        return 1
    rtl.set_rate(FS)
    rtl.set_ppm(args.ppm)
    if args.gain is None:
        rtl.set_gain_auto()
    else:
        rtl.set_gain(int(round(args.gain * 10)))
    time.sleep(0.2)
    print(f"# rtl_tcp {args.host}:{args.port}  tuner={rtl.tuner_type}  fs={FS}  "
          f"nominal={NOMINAL_HZ/1e6:.3f} MHz  ppm={args.ppm}")

    # --once: passive single measurement, no station driving.
    if args.once:
        r = measure(rtl, NOMINAL_HZ, capture_s=2.0)
        print(fmt_row("on-air", r, None))
        rtl.close()
        return 0 if r else 2

    ids: list[int] = []
    if args.station is not None:
        ids = [args.station]
    elif args.stations:
        ids = [int(x) for x in args.stations.split(",") if x.strip()]
    else:
        print("! give --once, --station, or --stations", file=sys.stderr)
        rtl.close()
        return 1

    # Console port: the Instructor (for --via) or each station's own USB port.
    portmap = identify()
    instr_console = None
    if args.via is not None:
        if args.via not in portmap:
            print(f"! Instructor id {args.via} not found on USB", file=sys.stderr)
            rtl.close()
            return 1
        instr_console = serial.Serial(portmap[args.via], BAUD, timeout=0.2)
        time.sleep(0.3)
        print(f"# relaying via Instructor {args.via} on {portmap[args.via]}")

    print(f"# keying txcw {args.secs}s, {args.repeat}x each\n")
    table: dict[int, dict | None] = {}
    for sid in ids:
        if args.via is not None:
            console, prefix = instr_console, f"relay {sid} "
        elif sid in portmap:
            console = serial.Serial(portmap[sid], BAUD, timeout=0.2)
            time.sleep(0.3)
            prefix = ""
        else:
            print(f"  {sid:>10}   ! not on USB and no --via", file=sys.stderr)
            table[sid] = None
            continue

        reads = key_and_measure(rtl, console, prefix, args.secs, args.repeat)
        if args.via is None:
            console.close()
        # Aggregate repeats: median freq, worst jitter.
        if reads:
            freqs = np.array([r["freq_hz"] for r in reads])
            best = dict(reads[int(np.argmax([r["snr_db"] for r in reads]))])
            best["freq_hz"] = float(np.median(freqs))
            best["drift_hz"] = best["freq_hz"] - NOMINAL_HZ
            best["ppm"] = best["drift_hz"] / NOMINAL_HZ * 1e6
            if len(reads) > 1:
                best["jitter_hz"] = float(np.std(freqs))
            table[sid] = best
        else:
            table[sid] = None
        print(fmt_row(str(sid), table[sid], None))

    if instr_console is not None:
        instr_console.close()
    rtl.close()

    # Relative table against --ref (cancels the dongle's common-mode offset).
    if args.ref is not None and table.get(args.ref):
        ref_hz = table[args.ref]["freq_hz"]
        print(f"\n# relative to station {args.ref} ({ref_hz/1e6:.6f} MHz):")
        for sid in ids:
            print(fmt_row(str(sid), table.get(sid), ref_hz))
    return 0


if __name__ == "__main__":
    sys.exit(main())
