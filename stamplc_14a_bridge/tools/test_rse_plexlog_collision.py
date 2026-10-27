#!/usr/bin/env python3
"""Two-round live RSE/PlexLog collision test for the V1.0.9 gateway.

Each RSE command is sent immediately after a new PlexLog request is observed,
then the inverter write/readback and gateway counters are checked.  The script
always returns control to the physical RSE input before closing the USB port.
"""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial


GATEWAY_RE = re.compile(
    r"PLEXLOG GATEWAY=(?P<enabled>\w+).*?RX=(?P<rx>\d+).*?"
    r"FORWARDED=(?P<forwarded>\d+).*?CACHE=(?P<cache>\d+).*?"
    r"QUEUED=(?P<queued>\d+).*?INVALID=(?P<invalid>\d+).*?"
    r"DROPPED=(?P<dropped>\d+).*?EXPIRED=(?P<expired>\d+).*?"
    r"DOWNFAIL=(?P<downfail>\d+).*?MAXQ=(?P<maxq>\d+).*?"
    r"LASTMS=(?P<lastms>\d+).*?MAXMS=(?P<maxms>\d+).*?"
    r"REJECTED=(?P<rejected>\d+).*?CONTROLHITS=(?P<controlhits>\d+).*?"
    r"RESCUED=(?P<rescued>\d+).*?EXP_ID=(?P<exp_id>\d+).*?"
    r"EXP_FC=(?P<exp_fc>\d+).*?EXP_ADDR=(?P<exp_addr>\d+).*?"
    r"EXP_QTY=(?P<exp_qty>\d+).*?EXP_AGE=(?P<exp_age>\d+)"
)


def read_command(port: serial.Serial, command: str, timeout: float) -> str:
    port.reset_input_buffer()
    port.write((command + "\r\n").encode("ascii"))
    port.flush()
    deadline = time.monotonic() + timeout
    last_data = time.monotonic()
    data = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            data.extend(chunk)
            last_data = time.monotonic()
            text = data.decode("utf-8", "replace")
            if command == "show" and "=========================" in text:
                break
            if command.startswith("test ") and (
                "TEST STATUS=ACTIVE" in text or "TEST STATUS=REJECTED" in text
            ):
                break
            if command == "dryrun off CONFIRM" and (
                "RESULT " in text or "ERROR LIVE" in text
            ):
                break
        elif (
            data
            and command not in ("show", "dryrun off CONFIRM")
            and not command.startswith("test ")
            and time.monotonic() - last_data >= 0.35
        ):
            break
    return data.decode("utf-8", "replace")


def gateway_status(port: serial.Serial) -> tuple[dict[str, int], str]:
    text = read_command(port, "show", 2.5)
    match = GATEWAY_RE.search(text)
    if not match:
        raise RuntimeError("extended V1.0.9 PlexLog status was not returned")
    values = {
        key: int(value)
        for key, value in match.groupdict().items()
        if key != "enabled"
    }
    rse = re.search(r"RSE DI mask: 0x([0-9A-Fa-f]+)\s+level: ([^\r\n]+)", text)
    if not rse:
        raise RuntimeError("RSE state was not returned")
    return values, rse.group(2).strip()


def wait_for_next_plexlog_window(
    port: serial.Serial, last_rx: int, timeout: float = 30.0
) -> tuple[dict[str, int], float]:
    """Arm after 1.5 s of bus quiet, then detect the first new request."""
    deadline = time.monotonic() + timeout
    quiet_since = time.monotonic()
    observed_rx = last_rx
    armed = False
    while time.monotonic() < deadline:
        status, _ = gateway_status(port)
        now = time.monotonic()
        current_rx = status["rx"]
        if current_rx != observed_rx:
            observed_rx = current_rx
            quiet_since = now
            if armed:
                return status, now
        elif not armed and now - quiet_since >= 1.5:
            armed = True
        time.sleep(0.12)
    raise TimeoutError("no new PlexLog request arrived within 30 seconds")


def counter_delta(current: dict[str, int], start: dict[str, int], key: str) -> int:
    return current[key] - start[key]


def run(port_name: str) -> int:
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = 0.08
    port.write_timeout = 2
    port.dtr = False
    port.rts = False
    port.open()
    time.sleep(1.0)

    failed = False
    start: dict[str, int] | None = None
    try:
        start, physical = gateway_status(port)
        if physical != "100%":
            raise RuntimeError(f"physical RSE is {physical}, expected 100%")
        print(
            f"BASE RX={start['rx']} FWD={start['forwarded']} CACHE={start['cache']} "
            f"DOWNFAIL={start['downfail']} DROP={start['dropped']} "
            f"EXPIRED={start['expired']} MAXMS={start['maxms']}",
            flush=True,
        )

        sequence = [100, 60, 30, 0, 100] * 2
        last = start
        for index, level in enumerate(sequence, 1):
            round_no = 1 if index <= 5 else 2
            step_no = ((index - 1) % 5) + 1
            window, detected_at = wait_for_next_plexlog_window(port, last["rx"])
            command_started = time.monotonic()
            reply = read_command(port, f"test {level} CONFIRM", 8.0)
            collision_delay_ms = int((command_started - detected_at) * 1000)
            write_ok = bool(
                re.search(r"WRITE ID=2 .*?STATUS=OK", reply)
                and f"verified at {level}%" in reply
                and "TEST STATUS=ACTIVE" in reply
            )
            rejected = "TEST STATUS=REJECTED" in reply
            time.sleep(0.35)
            current, physical = gateway_status(port)
            # A deliberately collided upstream request may reach its bounded
            # queue deadline.  Record that as the test result and continue so
            # recovery can be measured.  Stop only for an inverter/downstream
            # failure, a rejected RSE command, or a failed write/readback.
            downstream_fault = counter_delta(current, start, "downfail") > 0
            print(
                f"ROUND={round_no} STEP={step_no}/5 LEVEL={level}% "
                f"COLLISION_DELAY={collision_delay_ms}ms WRITE={'OK' if write_ok else 'FAIL'} "
                f"RX={current['rx']} FWD={current['forwarded']} CACHE={current['cache']} "
                f"DOWNFAIL={current['downfail']} DROP={current['dropped']} "
                f"EXPIRED={current['expired']} MAXQ={current['maxq']} "
                f"MAXMS={current['maxms']} CONTROLHITS={current['controlhits']}",
                f"RESCUED={current['rescued']} LASTEXP="
                f"{current['exp_id']}/{current['exp_fc']}/"
                f"{current['exp_addr']}/{current['exp_qty']}/{current['exp_age']}ms",
                flush=True,
            )
            if rejected or not write_ok or physical != "100%" or downstream_fault:
                print("ABORT: safety/readback/gateway check failed", flush=True)
                print(reply.strip(), flush=True)
                failed = True
                break
            last = current

        final, _ = gateway_status(port)
        print(
            "TOTAL "
            + " ".join(
                f"d{key.upper()}={counter_delta(final, start, key)}"
                for key in (
                    "rx",
                    "forwarded",
                    "cache",
                    "downfail",
                    "dropped",
                    "expired",
                    "rejected",
                    "controlhits",
                    "rescued",
                )
            )
            + f" MAXQ={final['maxq']} MAXMS={final['maxms']} "
            + f"LASTEXP={final['exp_id']}/{final['exp_fc']}/"
            + f"{final['exp_addr']}/{final['exp_qty']}/{final['exp_age']}ms",
            flush=True,
        )
    except Exception as exc:
        failed = True
        print(f"ERROR: {exc}", flush=True)
    finally:
        try:
            reply = read_command(port, "dryrun off CONFIRM", 8.0)
            cleanup_ok = "verified at 100%" in reply and "WARNING LIVE CONTROL ENABLED" in reply
            print(f"RETURN_TO_LIVE_100={'OK' if cleanup_ok else 'CHECK'}", flush=True)
            if not cleanup_ok:
                print(reply.strip(), flush=True)
                failed = True
        finally:
            port.close()
    return 1 if failed else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3")
    args = parser.parse_args()
    sys.exit(run(args.port))
