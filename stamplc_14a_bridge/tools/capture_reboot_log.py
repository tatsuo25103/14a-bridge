"""Read-only SmartPLC reboot log capture.

Retries opening the USB serial port without toggling DTR/RTS and prints raw
boot/runtime lines with elapsed timestamps. It never sends a command.
"""

from __future__ import annotations

import argparse
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=30.0)
    args = parser.parse_args()
    started = time.monotonic()
    port: serial.Serial | None = None
    carry = bytearray()
    while time.monotonic() - started < args.seconds:
        if port is None:
            try:
                candidate = serial.Serial()
                candidate.port = args.port
                candidate.baudrate = 115200
                candidate.timeout = 0.05
                candidate.write_timeout = 0.2
                candidate.dsrdtr = False
                candidate.rtscts = False
                candidate.dtr = False
                candidate.rts = False
                candidate.open()
                port = candidate
                print(f"[{time.monotonic() - started:7.3f}] PORT OPEN", flush=True)
            except (OSError, serial.SerialException):
                time.sleep(0.05)
                continue
        try:
            chunk = port.read(port.in_waiting or 1)
            if not chunk:
                continue
            carry.extend(chunk)
            while b"\n" in carry:
                raw, _, carry = carry.partition(b"\n")
                line = raw.rstrip(b"\r").decode("utf-8", "replace")
                print(f"[{time.monotonic() - started:7.3f}] {line}", flush=True)
        except (OSError, serial.SerialException) as exc:
            print(f"[{time.monotonic() - started:7.3f}] PORT LOST: {exc}", flush=True)
            try:
                port.close()
            except Exception:
                pass
            port = None
    if port is not None:
        port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
