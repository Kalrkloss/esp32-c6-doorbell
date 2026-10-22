#!/usr/bin/env python3
"""Flash the doorbell board while it is awake.

A deep-sleeping ESP32-C6 powers its USB PHY down, so /dev/ttyACM* disappears.
This helper power-cycles the board through uhubctl and flashes in the short
window between power-on and the first deep sleep. esptool is used in-process so
that only the port open/connect latency has to fit into the window; once the
stub loader runs, the chip stays awake for the rest of the transfer.

Usage:
    python3 scripts/flash_when_awake.py --port /dev/ttyACM2 --hub 1-5 --hub-port 4
"""

import argparse
import os
import subprocess
import sys
import time

import esptool

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def flash_argv(port: str) -> list[str]:
    build = os.path.join(PROJECT, "build")
    return [
        "--chip", "esp32c6",
        "-p", port,
        "-b", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "4MB",
        "0x0", os.path.join(build, "bootloader", "bootloader.bin"),
        "0x8000", os.path.join(build, "partition_table", "partition-table.bin"),
        "0xf000", os.path.join(build, "ota_data_initial.bin"),
        "0x20000", os.path.join(build, "esp32c6_zigbee_doorbell.bin"),
    ]


def try_flash(argv: list[str]) -> bool:
    """Runs esptool in-process; returns True when the transfer verified."""
    try:
        esptool.main(argv)
        return True
    except SystemExit as exc:
        return exc.code in (0, None)
    except Exception as exc:  # noqa: BLE001 - the port can vanish mid-connect
        print(f"    attempt failed: {type(exc).__name__}: {exc}", flush=True)
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM2")
    ap.add_argument("--hub", default="1-5", help="uhubctl hub location, e.g. 1-5")
    ap.add_argument("--hub-port", default="4", help="port on that hub")
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    argv = flash_argv(args.port)
    deadline = time.time() + args.timeout
    attempt = 0

    while time.time() < deadline:
        attempt += 1
        print(f"[{attempt}] power-cycling {args.hub} port {args.hub_port}", flush=True)
        subprocess.run(
            ["uhubctl", "-l", args.hub, "-p", args.hub_port, "-a", "cycle", "-d", "1"],
            check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

        window_end = time.time() + 8.0
        while time.time() < window_end and time.time() < deadline:
            if not os.path.exists(args.port):
                time.sleep(0.02)
                continue
            if try_flash(argv):
                print("flash OK", flush=True)
                return 0
            time.sleep(0.1)

    print("flash FAILED: never caught the board awake", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
