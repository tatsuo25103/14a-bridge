"""Passive Modbus RTU bus monitor for shared-bus diagnostics."""

from __future__ import annotations

import argparse
import time
from datetime import datetime

import serial

from modbus_rtu import RTUFrameBuffer, hex_bytes


def describe(frame: bytes) -> str:
    device_id = frame[0]
    function = frame[1]

    if function & 0x80 and len(frame) == 5:
        return (
            f"EXCEPTION ID={device_id} FC=0x{function & 0x7F:02X} "
            f"code=0x{frame[2]:02X}"
        )

    if function == 0x03:
        if len(frame) == 8:
            address = int.from_bytes(frame[2:4], "big")
            quantity = int.from_bytes(frame[4:6], "big")
            return (
                f"FC03 REQUEST ID={device_id} "
                f"address=0x{address:04X} quantity={quantity}"
            )
        if len(frame) >= 5 and frame[2] == len(frame) - 5:
            return (
                f"FC03 RESPONSE ID={device_id} byte_count={frame[2]} "
                f"data={hex_bytes(frame[3:-2])}"
            )

    if function == 0x10:
        if len(frame) == 8:
            address = int.from_bytes(frame[2:4], "big")
            quantity = int.from_bytes(frame[4:6], "big")
            return (
                f"FC16 RESPONSE ID={device_id} "
                f"address=0x{address:04X} quantity={quantity}"
            )
        if len(frame) >= 9 and frame[6] == len(frame) - 9:
            address = int.from_bytes(frame[2:4], "big")
            quantity = int.from_bytes(frame[4:6], "big")
            return (
                f"FC16 REQUEST ID={device_id} "
                f"address=0x{address:04X} quantity={quantity} "
                f"data={hex_bytes(frame[7:-2])}"
            )

    return f"FRAME ID={device_id} FC=0x{function:02X}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Serial port, for example COM12")
    parser.add_argument("--baud", type=int, default=19200)
    parser.add_argument("--seconds", type=float, default=10.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    parser = RTUFrameBuffer()
    frame_count = 0
    deadline = time.monotonic() + args.seconds

    with serial.Serial(
        args.port,
        args.baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.05,
    ) as port:
        print(
            f"PASSIVE MONITOR {args.port} {args.baud} 8N1 "
            f"for {args.seconds:.1f} s",
            flush=True,
        )
        while time.monotonic() < deadline:
            waiting = port.in_waiting
            if not waiting:
                time.sleep(0.005)
                continue
            for event in parser.feed(port.read(waiting)):
                timestamp = datetime.now().strftime("%H:%M:%S.%f")
                if event.kind == "frame":
                    frame_count += 1
                    print(
                        f"[{timestamp}] {describe(event.data)} | "
                        f"{hex_bytes(event.data)}",
                        flush=True,
                    )
                elif event.data:
                    print(
                        f"[{timestamp}] {event.kind.upper()} | "
                        f"{hex_bytes(event.data)}",
                        flush=True,
                    )

    print(f"MONITOR COMPLETE frames={frame_count}", flush=True)


if __name__ == "__main__":
    main()
