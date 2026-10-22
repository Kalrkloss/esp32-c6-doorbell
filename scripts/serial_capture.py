#!/usr/bin/env python3
"""Capture the doorbell's serial output across deep-sleep cycles.

Two traps this script avoids:

1. The USB-Serial-JTAG device disappears while the chip sleeps, so a plain
   `cat`/`idf.py monitor` loses output or exits. The port is reopened as soon
   as it reappears.
2. Do NOT use pyserial here. pyserial asserts DTR and RTS when it opens a port,
   and on the ESP32-C6 USB-Serial-JTAG bridge that is exactly the reset sequence
   (the same lines esptool toggles): the chip reboots with wake cause UNDEFINED,
   RTC memory is gone, and a real button press looks like a power-on reset. A
   raw file descriptor leaves the modem lines alone.

Usage: python3 scripts/serial_capture.py --port /dev/ttyACM2 --seconds 30 --out /tmp/boot.log
"""

import argparse
import os
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM2")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--out", default="/tmp/doorbell_serial.log")
    args = ap.parse_args()

    out = open(args.out, "wb")
    end = time.time() + args.seconds
    fd = None
    while time.time() < end:
        if fd is None:
            try:
                # O_NOCTTY: no controlling terminal, O_NONBLOCK: open even while
                # the device is still initialising. No termios/DTR/RTS changes.
                fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            except OSError:
                time.sleep(0.02)
                continue
        try:
            data = os.read(fd, 8192)
            if data:
                out.write(data)
                out.flush()
            else:
                time.sleep(0.02)
        except BlockingIOError:
            time.sleep(0.02)
        except OSError:
            try:
                os.close(fd)
            except OSError:
                pass
            fd = None
    if fd is not None:
        try:
            os.close(fd)
        except OSError:
            pass
    out.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
