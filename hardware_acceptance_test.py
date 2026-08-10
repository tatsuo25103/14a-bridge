"""Explicit hardware acceptance test for the InfiniSolar feed-in limit."""

from __future__ import annotations

import argparse
import time
from datetime import datetime

from serial_worker import SerialWorker


WRITE_SEQUENCE = (0, 5000, 10000, 15000)


def log(kind: str, message: str) -> None:
    timestamp = datetime.now().strftime("%H:%M:%S.%f")
    print(f"[{timestamp}] {kind}: {message}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Read ID 3 register 0x04E5 and optionally run the explicit "
            "0/5000/10000/15000 W write-readback acceptance sequence."
        )
    )
    parser.add_argument("port", help="Serial port, for example COM12")
    parser.add_argument("--device-id", type=int, default=3)
    parser.add_argument("--baud", type=int, default=19200)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument(
        "--write-sequence",
        action="store_true",
        help="Authorize the four physical write/readback tests.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    worker = SerialWorker(log)
    worker.connect(args.port, args.baud)
    print(
        f"CONNECTED {args.port} {args.baud} 8N1, ID {args.device_id}",
        flush=True,
    )

    try:
        initial = worker.read_power(args.device_id, args.timeout)
        print(f"INITIAL_READBACK={initial} W", flush=True)

        if not args.write_sequence:
            print("READ-ONLY TEST COMPLETE", flush=True)
            return

        for target in WRITE_SEQUENCE:
            print(f"--- WRITE {target} W ---", flush=True)
            worker.write_power(args.device_id, target, args.timeout)
            time.sleep(0.5)
            actual = worker.read_power(args.device_id, args.timeout)
            print(
                f"VERIFY target={target} W actual={actual} W",
                flush=True,
            )
            if actual != target:
                raise RuntimeError(
                    f"Readback mismatch: wrote {target} W, read {actual} W"
                )
    finally:
        worker.disconnect()
        print("DISCONNECTED", flush=True)


if __name__ == "__main__":
    main()
