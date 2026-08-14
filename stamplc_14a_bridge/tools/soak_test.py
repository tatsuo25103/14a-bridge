"""Read-only long-duration health monitor for a commissioned 14a Bridge.

The script sends only ``identify`` and ``gui``. It never probes or writes an
inverter and never changes SmartPLC configuration.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


IDENTITY = re.compile(
    r"IDENTITY PRODUCT=14A_BRIDGE MODEL=STAMPPLC VERSION=([^\s\r\n]+)", re.I
)
HEALTH = re.compile(
    r"@ HEALTH UPTIME=(\d+) HEAP=(\d+) MINHEAP=(\d+) RESET=(\d+)", re.I
)


def open_port(port_name: str) -> serial.Serial:
    # Set handshake lines before opening. PySerial's Windows defaults may
    # otherwise pulse DTR/RTS and reset an ESP32-S3, producing a false soak
    # failure and interrupting the controller unnecessarily.
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 0.15
    port.write_timeout = 0.5
    port.dsrdtr = False
    port.rtscts = False
    port.dtr = False
    port.rts = False
    port.open()
    time.sleep(0.12)
    port.reset_input_buffer()
    return port


def collect(port: serial.Serial, timeout: float = 2.0) -> tuple[str, dict[str, int]]:
    port.reset_input_buffer()
    port.write(b"identify\n")
    port.write(b"gui\n")
    deadline = time.monotonic() + timeout
    response = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            response.extend(chunk)
            text = response.decode("utf-8", "replace")
            if IDENTITY.search(text) and HEALTH.search(text):
                break
    text = response.decode("utf-8", "replace")
    identity = IDENTITY.search(text)
    health = HEALTH.search(text)
    if not identity or not health:
        raise RuntimeError("SmartPLC identity or HEALTH line was not received")
    return identity.group(1), {
        "uptime": int(health.group(1)),
        "heap": int(health.group(2)),
        "min_heap": int(health.group(3)),
        "reset_reason": int(health.group(4)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Read-only 14a Bridge soak monitor (close the GUI first)."
    )
    parser.add_argument("--port", required=True, help="SmartPLC COM port, e.g. COM5")
    parser.add_argument("--hours", type=float, default=24.0)
    parser.add_argument("--interval", type=float, default=60.0, help="seconds")
    parser.add_argument("--output", type=Path, default=Path("soak_test.csv"))
    args = parser.parse_args()
    if args.hours <= 0 or args.interval < 5:
        parser.error("hours must be > 0 and interval must be >= 5 seconds")

    deadline = time.monotonic() + args.hours * 3600
    previous_uptime: int | None = None
    failures = 0
    unexpected_resets = 0
    port: serial.Serial | None = None
    new_file = not args.output.exists()
    with args.output.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "timestamp_utc",
                "version",
                "uptime_s",
                "free_heap",
                "minimum_heap",
                "reset_reason",
                "result",
            ],
        )
        if new_file:
            writer.writeheader()
        while time.monotonic() < deadline:
            row = {
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "version": "",
                "uptime_s": "",
                "free_heap": "",
                "minimum_heap": "",
                "reset_reason": "",
                "result": "OK",
            }
            try:
                if port is None or not port.is_open:
                    port = open_port(args.port)
                version, health = collect(port)
                row.update(
                    version=version,
                    uptime_s=health["uptime"],
                    free_heap=health["heap"],
                    minimum_heap=health["min_heap"],
                    reset_reason=health["reset_reason"],
                )
                if previous_uptime is not None and health["uptime"] < previous_uptime:
                    unexpected_resets += 1
                    row["result"] = "RESET_DETECTED"
                previous_uptime = health["uptime"]
            except Exception as exc:  # serial removal and timeouts are test evidence
                failures += 1
                row["result"] = f"ERROR: {exc}"
                if port is not None:
                    try:
                        port.close()
                    except Exception:
                        pass
                    port = None
            writer.writerow(row)
            handle.flush()
            print(row)
            time.sleep(args.interval)

    if port is not None:
        port.close()

    print(f"complete: communication_failures={failures} unexpected_resets={unexpected_resets}")
    return 0 if failures == 0 and unexpected_resets == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
