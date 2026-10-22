#!/usr/bin/env python3
"""Capture the doorbell's serial output across deep-sleep cycles.

The USB-Serial-JTAG device disappears while the chip sleeps, so a plain
`cat`/monitor loses output. This helper reopens the port as soon as it
reappears and appends everything to a file.

Usage: python3 scripts/serial_capture.py --port /dev/ttyACM2 --seconds 30 --out /tmp/boot.log
"""

import argparse
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM2")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--out", default="/tmp/doorbell_serial.log")
    args = ap.parse_args()

    out = open(args.out, "wb")
    end = time.time() + args.seconds
    ser = None
    while time.time() < end:
        if ser is None:
            try:
                ser = serial.Serial(args.port, 115200, timeout=0.2)
                # pyserial asserts DTR/RTS on open; on the ESP32-C6 USB-Serial-
                # JTAG bridge that can hold the chip in reset, so release both.
                ser.dtr = False
                ser.rts = False
            except Exception:
                time.sleep(0.02)
                continue
        try:
            data = ser.read(8192)
            if data:
                out.write(data)
                out.flush()
        except Exception:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
    out.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
