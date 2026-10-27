"""Regression checks for the six-inverter PlexLog capacity assumptions.

These checks use the 2026-10-24 single-inverter trace (about 18 requests in a
17 second cycle, 99 live forwards and 92 cache replies over 191 requests).
They are a conservative sizing model, not a replacement for a physical
six-inverter acceptance test.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[1]
HEADER = (SOURCE_ROOT / "include" / "PlexLogGateway.h").read_text(encoding="utf-8")
GATEWAY_SOURCE = (SOURCE_ROOT / "src" / "PlexLogGateway.cpp").read_text(encoding="utf-8")
MODBUS_SOURCE = (SOURCE_ROOT / "src" / "ModbusRtuMaster.cpp").read_text(encoding="utf-8")


def constant(name: str) -> int:
    match = re.search(rf"\b{name}\s*=\s*(\d+)", HEADER)
    if not match:
        raise AssertionError(f"missing integer constant {name}")
    return int(match.group(1))


class PlexLogCapacityTests(unittest.TestCase):
    inverter_count = 6
    requests_per_inverter = 18
    poll_period_s = 17.0
    one_inverter_active_window_s = 2.0
    forward_ratio = 99 / 191
    observed_response_s = 0.0632

    def test_cache_covers_conservative_unique_working_set(self) -> None:
        required = self.inverter_count * self.requests_per_inverter
        self.assertGreaterEqual(constant("CACHE_ENTRIES"), required)

    def test_rse_transition_cache_covers_one_poll_cycle(self) -> None:
        self.assertGreaterEqual(
            constant("CONTROL_CACHE_TTL_MS"), int(self.poll_period_s * 1000)
        )

    def test_six_inverter_nominal_cycle_keeps_headroom(self) -> None:
        requests = self.inverter_count * self.requests_per_inverter
        downstream_s = requests * self.forward_ratio * self.observed_response_s
        paced_window_s = self.inverter_count * self.one_inverter_active_window_s
        self.assertLess(downstream_s, self.poll_period_s)
        self.assertLess(paced_window_s, self.poll_period_s)

    def test_queue_remains_short_lived(self) -> None:
        # A larger cache is safe; a large/old queue is not. Late Modbus
        # responses could otherwise be mistaken for the next transaction.
        self.assertLessEqual(constant("QUEUE_MAX_AGE_MS"), 350)
        self.assertLessEqual(constant("QUEUE_DEPTH"), 8)

    def test_queued_read_rechecks_newly_verified_cache(self) -> None:
        service = GATEWAY_SOURCE.split("bool PlexLogGateway::serviceOneQueued", 1)[1]
        service = service.split("bool PlexLogGateway::upstreamQuietFor", 1)[0]
        self.assertLess(service.index("replyFromCache(frame)"), service.index("forward(frame"))

    def test_normal_power_verify_starts_inside_queue_window(self) -> None:
        initial = re.search(
            r"POWER_VERIFY_INITIAL_SETTLE_MS\s*=\s*(\d+)", MODBUS_SOURCE
        )
        self.assertIsNotNone(initial)
        self.assertLess(int(initial.group(1)), constant("QUEUE_MAX_AGE_MS"))


if __name__ == "__main__":
    unittest.main()
