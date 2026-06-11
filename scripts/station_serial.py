"""Shared USB-serial helpers for the station debug scripts.

Centralises the one non-obvious thing about talking to a station over USB: how
the port behaves when you open it, which differs by board.

  * Native-USB boards (Heltec V4, Cardputer, RAK, Wio) present a /dev/cu.usbmodem*
    CDC. The ESP32-S3/nRF52 IS the USB device; opening the port does NOT reset
    the MCU, and the running firmware answers `show` immediately.

  * The Heltec V3 (and any CP2102/CP210x UART bridge) presents /dev/cu.usbserial*.
    Here DTR/RTS are wired to the ESP32 auto-reset circuit, so a default pyserial
    open REBOOTS the board. Counter-intuitively that reboot is required: opened
    without it (DTR/RTS held low) the V3's console comes up SILENT. So the only
    reliable read is "let the open reset it, then wait for the firmware to boot".

`open_console()` hides that difference: it always returns a port that is booted
and ready to accept commands. On a usbserial bridge that costs a reset (~3.5 s
boot) — unavoidable, and fine as long as it happens before anything time-critical
(e.g. a txcw carrier) is keyed.
"""
from __future__ import annotations

import glob
import time

import serial

BAUD = 115200

# Boot slack for a usbserial bridge after the open-time reset: covers radio +
# BLE init before the runtime console answers. Native boards skip the reset and
# only need a brief settle for any in-flight boot chatter to drain.
BRIDGE_BOOT_S = 3.5
NATIVE_SETTLE_S = 0.3


def usb_ports() -> list[str]:
    """All candidate station ports: native-USB (usbmodem*) + UART bridges
    (usbserial*, e.g. the Heltec V3)."""
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))


def is_uart_bridge(port: str) -> bool:
    """True for CP2102/CP210x bridge ports (Heltec V3), which reset on open."""
    return "usbserial" in port


def open_console(port: str, *, baud: int = BAUD, timeout: float = 0.1) -> serial.Serial:
    """Open a station console and return it booted and ready.

    usbserial* bridges reboot on open, so we wait out the firmware boot before
    handing the port back; usbmodem* boards do not reset and need only a short
    settle. Input is flushed so the first read starts clean.
    """
    s = serial.Serial(port, baud, timeout=timeout)
    time.sleep(BRIDGE_BOOT_S if is_uart_bridge(port) else NATIVE_SETTLE_S)
    try:
        s.reset_input_buffer()
    except Exception:
        pass
    return s
