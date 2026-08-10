"""Regression tests for protocol construction, parsing, and filtering."""

from __future__ import annotations

import sys
import types
import unittest

# The transport is dependency-injected in these tests. Allow protocol tests to
# run before the application's declared pyserial dependency is installed.
try:
    import serial  # noqa: F401
except ModuleNotFoundError:
    serial_stub = types.ModuleType("serial")
    serial_stub.Serial = None
    serial_stub.SerialException = OSError
    serial_stub.EIGHTBITS = 8
    serial_stub.PARITY_NONE = "N"
    serial_stub.STOPBITS_ONE = 1
    sys.modules["serial"] = serial_stub

from modbus_rtu import (
    RTUFrameBuffer,
    append_crc,
    build_fc03_request,
    build_fc16_request,
    build_fc16_registers_request,
    classify_over_limit_result,
    classify_fc16_response,
    match_fc03_register_response,
    match_fc16_response,
    parse_fc03_registers,
    parse_fc03_u32,
    verify_crc,
)
from serial_worker import SerialWorker


class FakeSerial:
    def __init__(self, response: bytes) -> None:
        self.is_open = True
        self.port = "FAKE"
        self._response = response
        self._rx = bytearray()
        self.written = b""

    @property
    def in_waiting(self) -> int:
        return len(self._rx)

    def reset_input_buffer(self) -> None:
        self._rx.clear()

    def reset_output_buffer(self) -> None:
        pass

    def write(self, data: bytes) -> int:
        self.written = data
        self._rx.extend(self._response)
        return len(data)

    def flush(self) -> None:
        pass

    def read(self, size: int) -> bytes:
        result = bytes(self._rx[:size])
        del self._rx[:size]
        return result


class ProtocolTests(unittest.TestCase):
    def test_build_fc03_request(self) -> None:
        request = build_fc03_request(3)
        self.assertEqual(request[:-2], bytes.fromhex("03 03 04 E5 00 02"))
        self.assertTrue(verify_crc(request))

    def test_build_fc16_5000_w_request(self) -> None:
        request = build_fc16_request(3, 5000)
        self.assertEqual(
            request[:-2],
            bytes.fromhex("03 10 04 E5 00 02 04 00 00 13 88"),
        )
        self.assertTrue(verify_crc(request))

    def test_build_single_register_fc16_charge_voltage(self) -> None:
        request = build_fc16_registers_request(
            2,
            0x026F,
            (565,),
        )
        self.assertEqual(
            request[:-2],
            bytes.fromhex("02 10 02 6F 00 01 02 02 35"),
        )
        self.assertTrue(verify_crc(request))

    def test_write_single_charge_voltage_register(self) -> None:
        response = append_crc(bytes.fromhex("02 10 02 70 00 01"))
        logs: list[tuple[str, str]] = []
        worker = SerialWorker(lambda kind, message: logs.append((kind, message)))
        worker._serial = FakeSerial(response)

        ack_type = worker.write_registers(
            2,
            0x0270,
            (540,),
            timeout=0.1,
        )

        self.assertEqual(ack_type, "standard")
        self.assertEqual(
            worker.last_request[:-2],
            bytes.fromhex("02 10 02 70 00 01 02 02 1C"),
        )
        self.assertEqual(worker.last_response, response)

    def test_parse_fc03_u32_high_word_first(self) -> None:
        response = append_crc(bytes.fromhex("03 03 04 00 00 13 88"))
        self.assertEqual(parse_fc03_u32(response), 5000)

    def test_read_version_registers_1208_and_1209(self) -> None:
        request = build_fc03_request(2, 1208, 2)
        self.assertEqual(
            request[:-2], bytes.fromhex("02 03 04 B8 00 02")
        )
        self.assertTrue(verify_crc(request))

        response = append_crc(bytes.fromhex("02 03 04 01 02 03 04"))
        self.assertTrue(match_fc03_register_response(response, 2, 2))
        self.assertEqual(
            parse_fc03_registers(response, expected_quantity=2),
            (0x0102, 0x0304),
        )

    def test_classify_over_limit_results(self) -> None:
        self.assertEqual(
            classify_over_limit_result(20000, 15000, 15000),
            "LIMIT_CLAMPED",
        )
        self.assertEqual(
            classify_over_limit_result(20000, 15000, 12000),
            "LIMIT_REDUCED",
        )
        self.assertEqual(
            classify_over_limit_result(20000, 15000, 20000),
            "ABOVE_LIMIT_RETAINED",
        )
        self.assertEqual(
            classify_over_limit_result(20000, 15000, 18000),
            "ABOVE_LIMIT_UNSTABLE",
        )

    def test_parser_handles_partial_and_concatenated_frames(self) -> None:
        first = append_crc(bytes.fromhex("02 10 09 CA 00 04"))
        second = append_crc(bytes.fromhex("02 10 04 E5 00 02"))
        parser = RTUFrameBuffer()
        self.assertEqual(parser.feed(first[:3]), [])
        events = parser.feed(first[3:] + second)
        frames = [event.data for event in events if event.kind == "frame"]
        self.assertEqual(frames, [first, second])

    def test_parser_resynchronizes_after_noise_and_bad_crc(self) -> None:
        corrupt = bytearray(append_crc(bytes.fromhex("02 10 09 CA 00 04")))
        corrupt[-1] ^= 0xFF
        valid = append_crc(bytes.fromhex("02 10 04 E5 00 02"))
        parser = RTUFrameBuffer()
        events = parser.feed(b"\x00\xff" + corrupt + valid)
        frames = [event.data for event in events if event.kind == "frame"]
        self.assertEqual(frames, [valid])
        self.assertTrue(any(event.kind != "frame" for event in events))

    def test_transaction_ignores_unrelated_valid_fc16_frame(self) -> None:
        bus_frame = append_crc(bytes.fromhex("02 10 09 CB 00 04"))
        expected = append_crc(bytes.fromhex("02 10 04 E5 00 02"))
        logs: list[tuple[str, str]] = []
        worker = SerialWorker(lambda kind, message: logs.append((kind, message)))
        worker._serial = FakeSerial(bus_frame + expected)

        response = worker.transact(
            build_fc16_request(2, 5000),
            lambda frame: match_fc16_response(frame, 2),
            timeout=0.1,
        )

        self.assertEqual(response, expected)
        self.assertTrue(any(kind == "BUS" for kind, _ in logs))
        self.assertTrue(any(kind == "RX" for kind, _ in logs))

    def test_byte_addressed_fc16_ack_requires_compatibility_mode(self) -> None:
        response = append_crc(bytes.fromhex("02 10 09 CA 00 04"))
        self.assertFalse(match_fc16_response(response, 2))
        self.assertTrue(
            match_fc16_response(
                response,
                2,
                allow_byte_addressed=True,
            )
        )
        self.assertEqual(
            classify_fc16_response(
                response,
                2,
                allow_byte_addressed=True,
            ),
            "byte_addressed",
        )

    def test_write_power_marks_byte_addressed_response_unverified(self) -> None:
        response = append_crc(bytes.fromhex("02 10 09 CA 00 04"))
        logs: list[tuple[str, str]] = []
        worker = SerialWorker(lambda kind, message: logs.append((kind, message)))
        worker._serial = FakeSerial(response)

        ack_type = worker.write_power(2, 5000, timeout=0.1)

        self.assertEqual(ack_type, "byte_addressed")
        self.assertEqual(worker.last_request, build_fc16_request(2, 5000))
        self.assertEqual(worker.last_response, response)
        self.assertTrue(any(kind == "RX" for kind, _ in logs))
        self.assertTrue(
            any(
                kind == "WARN"
                and "non-standard" in message
                and "not proof" in message
                for kind, message in logs
            )
        )
        self.assertFalse(any(kind == "BUS" for kind, _ in logs))


if __name__ == "__main__":
    unittest.main()
