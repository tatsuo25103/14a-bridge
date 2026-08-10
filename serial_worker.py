"""Serial transport and transaction matching for the InfiniSolar tool."""

from __future__ import annotations

import threading
import time
from collections.abc import Callable
from typing import Any

import serial

from modbus_rtu import (
    POWER_REGISTER,
    POWER_REGISTER_QUANTITY,
    RTUFrameBuffer,
    build_fc03_request,
    build_fc16_request,
    build_fc16_registers_request,
    classify_fc16_response,
    hex_bytes,
    match_fc03_register_response,
    match_fc03_response,
    match_fc16_response,
    parse_fc03_registers,
    parse_fc03_u32,
    raise_for_exception,
)


LogCallback = Callable[[str, str], None]
FrameMatcher = Callable[[bytes], bool]


class ResponseTimeout(TimeoutError):
    """No matching response arrived before the transaction deadline."""


class SerialWorker:
    def __init__(self, log_callback: LogCallback | None = None) -> None:
        self._serial: Any | None = None
        self._lock = threading.RLock()
        self._log_callback = log_callback or (lambda _kind, _message: None)
        self.last_request: bytes | None = None
        self.last_response: bytes | None = None

    @property
    def is_connected(self) -> bool:
        return self._serial is not None and bool(self._serial.is_open)

    @property
    def port(self) -> str | None:
        return self._serial.port if self.is_connected else None

    @property
    def baudrate(self) -> int | None:
        return int(self._serial.baudrate) if self.is_connected else None

    def connect(self, port: str, baudrate: int) -> None:
        with self._lock:
            if self.is_connected:
                self.disconnect()
            self._serial = serial.Serial(
                port=port,
                baudrate=baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.05,
                write_timeout=1.0,
            )

    def disconnect(self) -> None:
        with self._lock:
            if self._serial is not None:
                try:
                    if self._serial.is_open:
                        self._serial.close()
                finally:
                    self._serial = None

    def transact(
        self,
        request: bytes,
        matcher: FrameMatcher,
        timeout: float,
    ) -> bytes:
        if timeout <= 0:
            raise ValueError("Timeout must be greater than zero.")

        with self._lock:
            if not self.is_connected:
                raise serial.SerialException("Serial port is not connected.")

            port = self._serial
            parser = RTUFrameBuffer()
            self.last_request = request
            self.last_response = None
            port.reset_input_buffer()
            port.reset_output_buffer()
            self._emit("TX", f"[ID {request[0]}] {hex_bytes(request)}")
            port.write(request)
            port.flush()
            deadline = time.monotonic() + timeout

            while time.monotonic() < deadline:
                waiting = int(port.in_waiting)
                if waiting:
                    chunk = port.read(waiting)
                    for event in parser.feed(chunk):
                        if event.kind == "frame":
                            if matcher(event.data):
                                self.last_response = event.data
                                self._emit(
                                    "RX",
                                    f"[ID {event.data[0]}] "
                                    f"{hex_bytes(event.data)}",
                                )
                                return event.data
                            self._emit(
                                "BUS",
                                f"[ID {event.data[0]}] "
                                f"{hex_bytes(event.data)}",
                            )
                        elif event.data:
                            label = (
                                "CRC mismatch"
                                if event.kind == "bad_crc"
                                else "discarded bytes"
                            )
                            self._emit(
                                "WARN", f"{label}: {hex_bytes(event.data)}"
                            )
                else:
                    time.sleep(0.005)

            for event in parser.flush():
                if event.data:
                    label = (
                        "CRC mismatch"
                        if event.kind == "bad_crc"
                        else "incomplete/noise"
                    )
                    self._emit("WARN", f"{label}: {hex_bytes(event.data)}")

            self._emit("ERROR", "No matching response / timeout")
            raise ResponseTimeout(
                f"No matching Modbus response within {timeout:.1f} s."
            )

    def read_power(
        self,
        device_id: int,
        timeout: float,
        address: int = POWER_REGISTER,
    ) -> int:
        request = build_fc03_request(
            device_id, address, POWER_REGISTER_QUANTITY
        )
        response = self.transact(
            request,
            lambda frame: match_fc03_response(frame, device_id),
            timeout,
        )
        raise_for_exception(response)
        return parse_fc03_u32(response)

    def read_registers(
        self,
        device_id: int,
        address: int,
        quantity: int,
        timeout: float,
    ) -> tuple[int, ...]:
        request = build_fc03_request(device_id, address, quantity)
        response = self.transact(
            request,
            lambda frame: match_fc03_register_response(
                frame, device_id, quantity
            ),
            timeout,
        )
        raise_for_exception(response)
        return parse_fc03_registers(response, expected_quantity=quantity)

    def write_power(
        self,
        device_id: int,
        power: int,
        timeout: float,
        address: int = POWER_REGISTER,
    ) -> str:
        request = build_fc16_request(device_id, power, address)
        response = self.transact(
            request,
            lambda frame: match_fc16_response(
                frame,
                device_id,
                address,
                POWER_REGISTER_QUANTITY,
                allow_byte_addressed=True,
            ),
            timeout,
        )
        raise_for_exception(response)
        ack_type = classify_fc16_response(
            response,
            device_id,
            address,
            POWER_REGISTER_QUANTITY,
            allow_byte_addressed=True,
        )
        if ack_type == "byte_addressed":
            self._emit(
                "WARN",
                "Observed a related, non-standard P17 FC16 response: "
                f"register 0x{address:04X} -> byte address "
                f"0x{address * 2:04X}, quantity "
                f"{POWER_REGISTER_QUANTITY} registers -> "
                f"{POWER_REGISTER_QUANTITY * 2} bytes. "
                "This is not proof that the setting was applied; "
                "FC03 readback verification is required.",
            )
        return ack_type or "unknown"

    def write_registers(
        self,
        device_id: int,
        address: int,
        values: tuple[int, ...],
        timeout: float,
        *,
        allow_byte_addressed: bool = True,
    ) -> str:
        request = build_fc16_registers_request(
            device_id, address, values
        )
        quantity = len(values)
        response = self.transact(
            request,
            lambda frame: match_fc16_response(
                frame,
                device_id,
                address,
                quantity,
                allow_byte_addressed=allow_byte_addressed,
            ),
            timeout,
        )
        raise_for_exception(response)
        ack_type = classify_fc16_response(
            response,
            device_id,
            address,
            quantity,
            allow_byte_addressed=allow_byte_addressed,
        )
        if ack_type == "byte_addressed":
            self._emit(
                "WARN",
                "Observed a related, non-standard FC16 response: "
                f"register 0x{address:04X} -> byte address "
                f"0x{address * 2:04X}, quantity {quantity} registers -> "
                f"{quantity * 2} bytes. FC03 readback verification is "
                "required.",
            )
        return ack_type or "unknown"

    def _emit(self, kind: str, message: str) -> None:
        self._log_callback(kind, message)
