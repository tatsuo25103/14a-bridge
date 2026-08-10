"""Small Modbus RTU helpers used by the InfiniSolar application."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable


POWER_REGISTER = 0x04E5
POWER_REGISTER_QUANTITY = 2


class ModbusError(Exception):
    """Base class for protocol errors."""


class ModbusExceptionResponse(ModbusError):
    """A slave returned a Modbus exception response."""

    NAMES = {
        0x01: "Illegal Function",
        0x02: "Illegal Data Address",
        0x03: "Illegal Data Value",
        0x04: "Slave Device Failure",
        0x05: "Acknowledge",
        0x06: "Slave Device Busy",
        0x08: "Memory Parity Error",
        0x0A: "Gateway Path Unavailable",
        0x0B: "Gateway Target Failed to Respond",
    }

    def __init__(self, function_code: int, exception_code: int) -> None:
        self.function_code = function_code
        self.exception_code = exception_code
        description = self.NAMES.get(exception_code, "Unknown Exception")
        super().__init__(
            f"Modbus exception 0x{exception_code:02X}: {description}"
        )


def crc16(data: bytes | bytearray | memoryview) -> int:
    """Return the Modbus CRC-16 value for *data*."""
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def append_crc(data: bytes) -> bytes:
    value = crc16(data)
    return data + bytes((value & 0xFF, value >> 8))


def verify_crc(frame: bytes | bytearray | memoryview) -> bool:
    return len(frame) >= 4 and crc16(frame[:-2]) == int.from_bytes(
        frame[-2:], "little"
    )


def hex_bytes(data: bytes | bytearray | memoryview) -> str:
    return " ".join(f"{value:02X}" for value in data)


def validate_device_id(device_id: int) -> None:
    if not 1 <= device_id <= 247:
        raise ValueError("Inverter ID must be between 1 and 247.")


def classify_over_limit_result(
    requested: int,
    machine_max: int,
    final_readback: int,
) -> str:
    """Classify the final readback of an intentional over-limit test."""
    if requested <= machine_max:
        raise ValueError("Requested value is not above the machine maximum.")
    if final_readback == machine_max:
        return "LIMIT_CLAMPED"
    if final_readback <= machine_max:
        return "LIMIT_REDUCED"
    if final_readback == requested:
        return "ABOVE_LIMIT_RETAINED"
    return "ABOVE_LIMIT_UNSTABLE"


def build_fc03_request(
    device_id: int,
    address: int = POWER_REGISTER,
    quantity: int = POWER_REGISTER_QUANTITY,
) -> bytes:
    validate_device_id(device_id)
    if not 0 <= address <= 0xFFFF:
        raise ValueError("Register address must be between 0x0000 and 0xFFFF.")
    if not 1 <= quantity <= 125:
        raise ValueError("FC03 quantity must be between 1 and 125.")
    return append_crc(
        bytes(
            (
                device_id,
                0x03,
                address >> 8,
                address & 0xFF,
                quantity >> 8,
                quantity & 0xFF,
            )
        )
    )


def build_fc16_request(
    device_id: int,
    value: int,
    address: int = POWER_REGISTER,
) -> bytes:
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError("Power must be between 0 and 4294967295 W.")
    return build_fc16_registers_request(
        device_id,
        address,
        (
            (value >> 16) & 0xFFFF,
            value & 0xFFFF,
        ),
    )


def build_fc16_registers_request(
    device_id: int,
    address: int,
    values: Iterable[int],
) -> bytes:
    validate_device_id(device_id)
    if not 0 <= address <= 0xFFFF:
        raise ValueError("Register address must be between 0x0000 and 0xFFFF.")
    register_values = tuple(values)
    if not 1 <= len(register_values) <= 123:
        raise ValueError("FC16 quantity must be between 1 and 123.")
    if address + len(register_values) - 1 > 0xFFFF:
        raise ValueError("FC16 register range exceeds 0xFFFF.")
    if any(not 0 <= value <= 0xFFFF for value in register_values):
        raise ValueError("FC16 register values must be between 0 and 65535.")
    data = b"".join(value.to_bytes(2, "big") for value in register_values)
    quantity = len(register_values)
    return append_crc(
        bytes(
            (
                device_id,
                0x10,
                address >> 8,
                address & 0xFF,
                quantity >> 8,
                quantity & 0xFF,
                len(data),
            )
        )
        + data
    )


def is_exception_for(frame: bytes, device_id: int, function_code: int) -> bool:
    return (
        len(frame) == 5
        and frame[0] == device_id
        and frame[1] == (function_code | 0x80)
    )


def match_fc03_response(frame: bytes, device_id: int) -> bool:
    return match_fc03_register_response(
        frame, device_id, POWER_REGISTER_QUANTITY
    )


def match_fc03_register_response(
    frame: bytes,
    device_id: int,
    quantity: int,
) -> bool:
    byte_count = quantity * 2
    return (
        is_exception_for(frame, device_id, 0x03)
        or (
            len(frame) == byte_count + 5
            and frame[0] == device_id
            and frame[1] == 0x03
            and frame[2] == byte_count
        )
    )


def match_fc16_response(
    frame: bytes,
    device_id: int,
    address: int = POWER_REGISTER,
    quantity: int = POWER_REGISTER_QUANTITY,
    allow_byte_addressed: bool = False,
) -> bool:
    return (
        classify_fc16_response(
            frame,
            device_id,
            address,
            quantity,
            allow_byte_addressed,
        )
        is not None
    )


def classify_fc16_response(
    frame: bytes,
    device_id: int,
    address: int = POWER_REGISTER,
    quantity: int = POWER_REGISTER_QUANTITY,
    allow_byte_addressed: bool = False,
) -> str | None:
    """Classify a standard or observed P17 byte-addressed FC16 ACK."""
    if is_exception_for(frame, device_id, 0x10):
        return "exception"
    if len(frame) != 8 or frame[0] != device_id or frame[1] != 0x10:
        return None

    response_address = int.from_bytes(frame[2:4], "big")
    response_quantity = int.from_bytes(frame[4:6], "big")
    if response_address == address and response_quantity == quantity:
        return "standard"

    # The parallel master has been observed to acknowledge register writes
    # using an internal byte offset and byte count:
    # 0x04E5 * 2 = 0x09CA, 2 registers * 2 = 4 bytes.
    if (
        allow_byte_addressed
        and address <= 0x7FFF
        and response_address == address * 2
        and response_quantity == quantity * 2
    ):
        return "byte_addressed"
    return None


def raise_for_exception(frame: bytes) -> None:
    if len(frame) == 5 and frame[1] & 0x80:
        raise ModbusExceptionResponse(frame[1] & 0x7F, frame[2])


def parse_fc03_u32(frame: bytes) -> int:
    registers = parse_fc03_registers(frame, expected_quantity=2)
    return (registers[0] << 16) | registers[1]


def parse_fc03_registers(
    frame: bytes,
    expected_quantity: int | None = None,
) -> tuple[int, ...]:
    if len(frame) < 7 or frame[1] != 0x03:
        raise ModbusError("Invalid FC03 register response.")
    if not verify_crc(frame):
        raise ModbusError("FC03 response has an invalid CRC.")
    byte_count = frame[2]
    if byte_count < 2 or byte_count % 2 or len(frame) != byte_count + 5:
        raise ModbusError("Invalid FC03 byte count.")
    quantity = byte_count // 2
    if expected_quantity is not None and quantity != expected_quantity:
        raise ModbusError(
            f"Expected {expected_quantity} registers, received {quantity}."
        )
    return tuple(
        int.from_bytes(frame[index : index + 2], "big")
        for index in range(3, 3 + byte_count, 2)
    )


@dataclass(frozen=True)
class FrameEvent:
    """An extracted frame or discarded input."""

    kind: str  # "frame", "bad_crc", or "noise"
    data: bytes


class RTUFrameBuffer:
    """Incrementally extract CRC-valid Modbus RTU frames.

    The parser accepts partial frames, concatenated frames, unrelated frames,
    and arbitrary leading bytes. It recognizes the response shapes needed by
    this application plus common read/write requests observed on a shared bus.
    """

    _BYTE_COUNT_FUNCTIONS = frozenset((0x01, 0x02, 0x03, 0x04))
    _FIXED_EIGHT_FUNCTIONS = frozenset((0x05, 0x06, 0x0F, 0x10))
    _KNOWN_FUNCTIONS = _BYTE_COUNT_FUNCTIONS | _FIXED_EIGHT_FUNCTIONS

    def __init__(self, max_buffer: int = 1024) -> None:
        self._buffer = bytearray()
        self._max_buffer = max_buffer

    @property
    def pending(self) -> bytes:
        return bytes(self._buffer)

    def feed(self, data: bytes | bytearray | memoryview) -> list[FrameEvent]:
        self._buffer.extend(data)
        events: list[FrameEvent] = []

        while self._buffer:
            found = self._find_valid_frame()
            if found is not None:
                start, length = found
                if start:
                    discarded = bytes(self._buffer[:start])
                    events.append(
                        FrameEvent(self._discard_kind(discarded), discarded)
                    )
                    del self._buffer[:start]
                frame = bytes(self._buffer[:length])
                del self._buffer[:length]
                events.append(FrameEvent("frame", frame))
                continue

            keep_from = self._earliest_incomplete_start()
            if keep_from is None:
                discarded = bytes(self._buffer)
                self._buffer.clear()
                events.append(
                    FrameEvent(self._discard_kind(discarded), discarded)
                )
            elif keep_from:
                discarded = bytes(self._buffer[:keep_from])
                del self._buffer[:keep_from]
                events.append(
                    FrameEvent(self._discard_kind(discarded), discarded)
                )
            break

        if len(self._buffer) > self._max_buffer:
            excess = len(self._buffer) - self._max_buffer
            discarded = bytes(self._buffer[:excess])
            del self._buffer[:excess]
            events.append(FrameEvent("noise", discarded))

        return events

    def flush(self) -> list[FrameEvent]:
        if not self._buffer:
            return []
        discarded = bytes(self._buffer)
        self._buffer.clear()
        return [FrameEvent(self._discard_kind(discarded), discarded)]

    def _find_valid_frame(self) -> tuple[int, int] | None:
        data = self._buffer
        for start in range(len(data)):
            for length in self._candidate_lengths(start):
                end = start + length
                if end <= len(data) and verify_crc(data[start:end]):
                    return start, length
        return None

    def _earliest_incomplete_start(self) -> int | None:
        data = self._buffer
        for start, unit_id in enumerate(data):
            if not 1 <= unit_id <= 247:
                continue
            remaining = len(data) - start
            if remaining == 1:
                return start
            function_code = data[start + 1]
            if not self._is_known_function(function_code):
                continue
            lengths = self._candidate_lengths(start)
            if not lengths or any(length > remaining for length in lengths):
                return start
        return None

    def _candidate_lengths(self, start: int) -> list[int]:
        data = self._buffer
        remaining = len(data) - start
        if remaining < 2 or not 1 <= data[start] <= 247:
            return []

        function_code = data[start + 1]
        if function_code & 0x80:
            return [5] if self._is_known_function(function_code & 0x7F) else []

        if function_code in self._BYTE_COUNT_FUNCTIONS:
            lengths: list[int] = [8]  # Read request.
            if remaining >= 3 and 1 <= data[start + 2] <= 250:
                lengths.insert(0, 5 + data[start + 2])  # Read response.
            return self._unique(lengths)

        if function_code in (0x05, 0x06):
            return [8]

        if function_code in (0x0F, 0x10):
            lengths = [8]  # Write response.
            if remaining >= 7 and 1 <= data[start + 6] <= 246:
                lengths.append(9 + data[start + 6])  # Write request.
            return self._unique(lengths)

        return []

    @classmethod
    def _is_known_function(cls, function_code: int) -> bool:
        return (function_code & 0x7F) in cls._KNOWN_FUNCTIONS

    @staticmethod
    def _unique(values: Iterable[int]) -> list[int]:
        return list(dict.fromkeys(values))

    @staticmethod
    def _discard_kind(data: bytes) -> str:
        # Mark a plausible complete Modbus-shaped chunk as a CRC error.
        if len(data) >= 5 and 1 <= data[0] <= 247:
            function_code = data[1] & 0x7F
            if function_code in RTUFrameBuffer._KNOWN_FUNCTIONS:
                return "bad_crc"
        return "noise"
