"""Tkinter GUI for controlling the InfiniSolar P17 feed-in power limit."""

from __future__ import annotations

import threading
import time
import tkinter as tk
from datetime import datetime
from decimal import Decimal, InvalidOperation
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import serial.tools.list_ports

from modbus_rtu import (
    ModbusError,
    POWER_REGISTER,
    POWER_REGISTER_QUANTITY,
    append_crc,
    classify_over_limit_result,
    hex_bytes,
    verify_crc,
)
from serial_worker import SerialWorker


BAUD_DEFAULT = 19200
PRESET_POWERS = (0, 5000, 10000, 15000)
TIMEOUTS = (0.5, 1.0, 1.5, 2.0, 3.0)
VERSION_REGISTER_START = 1208
VERSION_REGISTER_QUANTITY = 2
CHARGE_CV_REGISTER = 0x026F
CHARGE_FLOAT_REGISTER = 0x0270
CHARGE_LIMIT_UPPER_REGISTER = 0x0107
CHARGE_LIMIT_LOWER_REGISTER = 0x0108


class InfiniSolarGUI:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("InfiniSolar IP21 RS485 Power Control")
        self.root.geometry("1080x820")
        self.root.minsize(900, 700)

        self.worker = SerialWorker(self._worker_log)
        self.operation_lock = threading.Lock()
        self.control_widgets: list[tk.Widget] = []
        self.action_widgets: list[tk.Widget] = []

        self.com_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(BAUD_DEFAULT))
        self.id_var = tk.StringVar(value="3")
        self.register_var = tk.StringVar(value=f"0x{POWER_REGISTER:04X}")
        self.timeout_var = tk.StringVar(value="1.0")
        self.machine_max_var = tk.StringVar(value="15000")
        self.custom_power_var = tk.StringVar(value="5000")
        self.status_var = tk.StringVar(value="Disconnected")
        self.current_power_var = tk.StringVar(value="-- W")
        self.write_result_var = tk.StringVar(value="No write yet")
        self.version_var = tk.StringVar(value="Not read")
        self.charge_cv_current_var = tk.StringVar(value="-- V")
        self.charge_float_current_var = tk.StringVar(value="-- V")
        self.charge_cv_set_var = tk.StringVar(value="56.5")
        self.charge_float_set_var = tk.StringVar(value="56.5")
        self.last_rd_report = ""

        self._create_widgets()
        self.root.protocol(
            "WM_DELETE_WINDOW", lambda: self._ui(self._on_close)
        )
        self._ui(self._start_scan)

    def _create_widgets(self) -> None:
        outer = ttk.Frame(self.root, padding=10)
        outer.pack(fill="both", expand=True)

        connection = ttk.LabelFrame(
            outer, text="RS485 Connection", padding=10
        )
        connection.pack(fill="x", pady=(0, 8))
        connection.columnconfigure(8, weight=1)

        ttk.Label(connection, text="COM Port").grid(row=0, column=0, padx=4)
        self.com_box = ttk.Combobox(
            connection, textvariable=self.com_var, width=14, state="readonly"
        )
        self.com_box.grid(row=0, column=1, padx=4)

        self.scan_button = ttk.Button(
            connection,
            text="Scan",
            command=lambda: self._ui(self._start_scan),
        )
        self.scan_button.grid(row=0, column=2, padx=4)

        ttk.Label(connection, text="Baud").grid(row=0, column=3, padx=4)
        self.baud_box = ttk.Combobox(
            connection,
            textvariable=self.baud_var,
            values=(9600, 19200, 38400),
            width=9,
            state="readonly",
        )
        self.baud_box.grid(row=0, column=4, padx=4)

        self.connect_button = ttk.Button(
            connection,
            text="Connect",
            command=lambda: self._ui(self._start_connect),
        )
        self.connect_button.grid(row=0, column=5, padx=4)

        self.disconnect_button = ttk.Button(
            connection,
            text="Disconnect",
            command=lambda: self._ui(self._start_disconnect),
        )
        self.disconnect_button.grid(row=0, column=6, padx=4)

        self.status_label = tk.Label(
            connection,
            textvariable=self.status_var,
            foreground="#b00020",
            anchor="w",
        )
        self.status_label.grid(row=1, column=0, columnspan=9, sticky="w", pady=(8, 0))

        settings = ttk.LabelFrame(
            outer, text="Modbus Transaction", padding=10
        )
        settings.pack(fill="x", pady=(0, 8))

        ttk.Label(settings, text="Inverter ID").grid(row=0, column=0, padx=4)
        self.id_box = ttk.Spinbox(
            settings,
            from_=1,
            to=247,
            textvariable=self.id_var,
            width=8,
        )
        self.id_box.grid(row=0, column=1, padx=4)

        ttk.Label(settings, text="Register").grid(row=0, column=2, padx=4)
        ttk.Entry(
            settings,
            textvariable=self.register_var,
            width=11,
            state="readonly",
        ).grid(row=0, column=3, padx=4)

        ttk.Label(settings, text="Read FC03 / Write FC16").grid(
            row=0, column=4, padx=12
        )
        ttk.Label(settings, text="Timeout").grid(row=0, column=5, padx=4)
        self.timeout_box = ttk.Combobox(
            settings,
            textvariable=self.timeout_var,
            values=TIMEOUTS,
            width=7,
            state="readonly",
        )
        self.timeout_box.grid(row=0, column=6, padx=4)

        ttk.Label(settings, text="Machine max W").grid(
            row=0, column=7, padx=(12, 4)
        )
        self.machine_max_entry = ttk.Entry(
            settings,
            textvariable=self.machine_max_var,
            width=10,
        )
        self.machine_max_entry.grid(row=0, column=8, padx=4)

        power = ttk.LabelFrame(
            outer, text="Feed-in Power Limit", padding=10
        )
        power.pack(fill="x", pady=(0, 8))

        for column, watts in enumerate(PRESET_POWERS):
            button = ttk.Button(
                power,
                text=f"{watts} W",
                command=lambda value=watts: self._ui(
                    self._confirm_write, value
                ),
            )
            button.grid(row=0, column=column, padx=5, pady=4)
            self.control_widgets.append(button)
            self.action_widgets.append(button)

        ttk.Separator(power, orient="vertical").grid(
            row=0, column=4, sticky="ns", padx=8
        )
        ttk.Label(power, text="Custom").grid(row=0, column=5, padx=4)
        self.custom_entry = ttk.Entry(
            power, textvariable=self.custom_power_var, width=12
        )
        self.custom_entry.grid(row=0, column=6, padx=4)
        self.custom_button = ttk.Button(
            power,
            text="Write",
            command=lambda: self._ui(self._confirm_custom_write),
        )
        self.custom_button.grid(row=0, column=7, padx=4)
        self.read_button = ttk.Button(
            power,
            text="Read Current",
            command=lambda: self._ui(self._start_read),
        )
        self.read_button.grid(row=0, column=8, padx=(15, 4))

        ttk.Label(power, text="Current:").grid(row=1, column=0, pady=(10, 0))
        ttk.Label(
            power,
            textvariable=self.current_power_var,
            font=("Segoe UI", 13, "bold"),
        ).grid(row=1, column=1, columnspan=2, sticky="w", pady=(10, 0))
        ttk.Label(power, text="Last write:").grid(
            row=1, column=4, pady=(10, 0)
        )
        ttk.Label(
            power,
            textvariable=self.write_result_var,
            font=("Segoe UI", 11, "bold"),
        ).grid(
            row=1,
            column=5,
            columnspan=4,
            sticky="w",
            pady=(10, 0),
        )

        version = ttk.LabelFrame(
            outer, text="Firmware Version Registers", padding=10
        )
        version.pack(fill="x", pady=(0, 8))
        self.version_button = ttk.Button(
            version,
            text="Read Version (1208 / 1209)",
            command=lambda: self._ui(self._start_version_read),
        )
        self.version_button.grid(row=0, column=0, padx=(0, 12))
        ttk.Label(
            version,
            textvariable=self.version_var,
            anchor="w",
        ).grid(row=0, column=1, sticky="w")

        charging = ttk.LabelFrame(
            outer, text="Battery Charging Voltages (P17)", padding=10
        )
        charging.pack(fill="x", pady=(0, 8))
        ttk.Label(charging, text="Parameter").grid(
            row=0, column=0, sticky="w", padx=4
        )
        ttk.Label(charging, text="Register").grid(
            row=0, column=1, padx=8
        )
        ttk.Label(charging, text="Current").grid(
            row=0, column=2, padx=8
        )
        ttk.Label(charging, text="Set value (V)").grid(
            row=0, column=3, padx=8
        )

        ttk.Label(charging, text="Constant / C.V. voltage").grid(
            row=1, column=0, sticky="w", padx=4, pady=3
        )
        ttk.Label(charging, text="0x026F").grid(
            row=1, column=1, padx=8
        )
        ttk.Label(
            charging, textvariable=self.charge_cv_current_var
        ).grid(row=1, column=2, padx=8)
        self.charge_cv_entry = ttk.Entry(
            charging, textvariable=self.charge_cv_set_var, width=10
        )
        self.charge_cv_entry.grid(row=1, column=3, padx=8)
        self.charge_cv_write_button = ttk.Button(
            charging,
            text="Write C.V.",
            command=lambda: self._ui(
                self._confirm_charge_voltage_write, "cv"
            ),
        )
        self.charge_cv_write_button.grid(row=1, column=4, padx=6)

        ttk.Label(charging, text="Floating voltage").grid(
            row=2, column=0, sticky="w", padx=4, pady=3
        )
        ttk.Label(charging, text="0x0270").grid(
            row=2, column=1, padx=8
        )
        ttk.Label(
            charging, textvariable=self.charge_float_current_var
        ).grid(row=2, column=2, padx=8)
        self.charge_float_entry = ttk.Entry(
            charging, textvariable=self.charge_float_set_var, width=10
        )
        self.charge_float_entry.grid(row=2, column=3, padx=8)
        self.charge_float_write_button = ttk.Button(
            charging,
            text="Write Floating",
            command=lambda: self._ui(
                self._confirm_charge_voltage_write, "float"
            ),
        )
        self.charge_float_write_button.grid(row=2, column=4, padx=6)

        self.charge_read_button = ttk.Button(
            charging,
            text="Read Both",
            command=lambda: self._ui(self._start_charge_voltage_read),
        )
        self.charge_read_button.grid(
            row=1, column=5, rowspan=2, padx=(18, 4)
        )
        ttk.Label(
            charging,
            text="Unit: 0.1 V/register; write FC16, read FC03",
        ).grid(row=3, column=0, columnspan=6, sticky="w", padx=4, pady=(5, 0))

        log_frame = ttk.LabelFrame(
            outer, text="Communication Log", padding=6
        )
        log_frame.pack(fill="both", expand=True)
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

        self.log_text = tk.Text(
            log_frame,
            wrap="none",
            font=("Consolas", 10),
            state="disabled",
        )
        self.log_text.grid(row=0, column=0, sticky="nsew")
        scrollbar = ttk.Scrollbar(
            log_frame, orient="vertical", command=self.log_text.yview
        )
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=scrollbar.set)
        self.log_text.tag_configure("TX", foreground="#005bbb")
        self.log_text.tag_configure("RX", foreground="#087f23")
        self.log_text.tag_configure("BUS", foreground="#b35a00")
        self.log_text.tag_configure("ERROR", foreground="#b00020")
        self.log_text.tag_configure("WARN", foreground="#8a5a00")
        self.log_text.tag_configure("INFO", foreground="#444444")

        self.clear_log_button = ttk.Button(
            log_frame,
            text="Clear Log",
            command=lambda: self._ui(self._clear_log),
        )
        self.clear_log_button.grid(
            row=1, column=0, sticky="w", pady=(6, 0)
        )

        report_buttons = ttk.Frame(log_frame)
        report_buttons.grid(row=1, column=0, sticky="e", pady=(6, 0))
        self.view_report_button = ttk.Button(
            report_buttons,
            text="View RD Report",
            command=lambda: self._ui(self._view_rd_report),
            state="disabled",
        )
        self.view_report_button.pack(side="left", padx=3)
        self.copy_report_button = ttk.Button(
            report_buttons,
            text="Copy RD Report",
            command=lambda: self._ui(self._copy_rd_report),
            state="disabled",
        )
        self.copy_report_button.pack(side="left", padx=3)
        self.save_report_button = ttk.Button(
            report_buttons,
            text="Save RD Report",
            command=lambda: self._ui(self._save_rd_report),
            state="disabled",
        )
        self.save_report_button.pack(side="left", padx=3)

        self.control_widgets.extend(
            (
                self.read_button,
                self.custom_button,
                self.custom_entry,
                self.id_box,
                self.timeout_box,
                self.machine_max_entry,
                self.version_button,
                self.charge_cv_entry,
                self.charge_float_entry,
                self.charge_cv_write_button,
                self.charge_float_write_button,
                self.charge_read_button,
            )
        )
        self.action_widgets.extend(
            (
                self.read_button,
                self.custom_button,
                self.version_button,
                self.charge_cv_write_button,
                self.charge_float_write_button,
                self.charge_read_button,
            )
        )
        self._refresh_connection_controls()

    def _ui(self, callback, *args) -> None:
        self.root.after(0, callback, *args)

    def _worker_log(self, kind: str, message: str) -> None:
        self._ui(self._append_log, kind, message)

    def _append_log(self, kind: str, message: str) -> None:
        markers = {"TX": "→ TX", "RX": "← RX", "BUS": "↔ BUS"}
        marker = markers.get(kind, kind)
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.log_text.configure(state="normal")
        self.log_text.insert(
            "end", f"[{timestamp}] {marker:<6} {message}\n", kind
        )
        self.log_text.configure(state="disabled")
        self.log_text.see("end")

    def _clear_log(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def _set_rd_report(self, report: str) -> None:
        self.last_rd_report = report
        for button in (
            self.view_report_button,
            self.copy_report_button,
            self.save_report_button,
        ):
            button.configure(state="normal")
        self._append_log(
            "INFO",
            "RD diagnostic report prepared. Use View, Copy, or Save RD Report.",
        )

    def _view_rd_report(self) -> None:
        if not self.last_rd_report:
            return
        window = tk.Toplevel(self.root)
        window.title("RD Diagnostic Report")
        window.geometry("900x650")
        text = tk.Text(window, wrap="word", font=("Consolas", 10))
        text.pack(fill="both", expand=True, padx=8, pady=8)
        text.insert("1.0", self.last_rd_report)
        text.configure(state="disabled")

    def _copy_rd_report(self) -> None:
        if not self.last_rd_report:
            return
        self.root.clipboard_clear()
        self.root.clipboard_append(self.last_rd_report)
        self.root.update_idletasks()
        self._append_log("INFO", "RD diagnostic report copied to clipboard.")

    def _save_rd_report(self) -> None:
        if not self.last_rd_report:
            return
        default_name = (
            "InfiniSolar_RD_Report_"
            + datetime.now().strftime("%Y%m%d_%H%M%S")
            + ".txt"
        )
        selected = filedialog.asksaveasfilename(
            parent=self.root,
            title="Save RD diagnostic report",
            defaultextension=".txt",
            initialfile=default_name,
            filetypes=(("Text files", "*.txt"), ("All files", "*.*")),
        )
        if not selected:
            return
        Path(selected).write_text(self.last_rd_report, encoding="utf-8")
        self._append_log("INFO", f"RD diagnostic report saved: {selected}")

    def _start_scan(self) -> None:
        self.scan_button.configure(state="disabled")
        threading.Thread(target=self._scan_worker, daemon=True).start()

    def _scan_worker(self) -> None:
        try:
            ports = [port.device for port in serial.tools.list_ports.comports()]
            self._ui(self._finish_scan, ports)
        except Exception as error:
            self._ui(self._finish_scan, [])
            self._ui(self._show_error, "COM scan failed", str(error))

    def _finish_scan(self, ports: list[str]) -> None:
        self.com_box.configure(values=ports)
        if ports and self.com_var.get() not in ports:
            self.com_var.set(ports[0])
        elif not ports:
            self.com_var.set("")
        self.scan_button.configure(
            state="disabled" if self.worker.is_connected else "normal"
        )
        self._append_log(
            "INFO",
            f"Found {len(ports)} COM port(s)."
            if ports
            else "No COM ports found.",
        )

    def _start_connect(self) -> None:
        port = self.com_var.get().strip()
        if not port:
            self._show_error("Connection", "Select a COM port first.")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            self._show_error("Connection", "Invalid baud rate.")
            return
        self._run_operation(
            lambda: self._connect_worker(port, baud),
            "Connecting...",
        )

    def _connect_worker(self, port: str, baud: int) -> None:
        self.worker.connect(port, baud)
        self._worker_log("INFO", f"Connected to {port} at {baud} baud, 8N1.")
        self._ui(self._set_connection_status, True, port)

    def _start_disconnect(self) -> None:
        self._run_operation(self._disconnect_worker, "Disconnecting...")

    def _disconnect_worker(self) -> None:
        port = self.worker.port or "serial port"
        self.worker.disconnect()
        self._worker_log("INFO", f"Disconnected from {port}.")
        self._ui(self._set_connection_status, False, "")

    def _confirm_custom_write(self) -> None:
        try:
            power = int(self.custom_power_var.get().strip())
        except ValueError:
            self._show_error("Power", "Power must be a whole number of watts.")
            return
        self._confirm_write(power)

    def _confirm_write(self, power: int) -> None:
        try:
            device_id, timeout = self._transaction_settings()
            machine_max = self._machine_max_power()
            if not 0 <= power <= 0xFFFFFFFF:
                raise ValueError("Power must be between 0 and 4294967295 W.")
        except ValueError as error:
            self._show_error("Invalid setting", str(error))
            return
        if not self.worker.is_connected:
            self._show_error("Write", "Connect to the inverter first.")
            return

        prompt = (
            "The following value will be written to the inverter:\n\n"
            f"Inverter ID: {device_id}\n"
            f"Register: 0x{POWER_REGISTER:04X}\n"
            f"Power: {power} W\n"
            f"Configured machine maximum: {machine_max} W\n"
            + (
                "\nWARNING: The requested value exceeds the configured "
                "machine maximum. This diagnostic may prove that the "
                "register stores the value, but it does not make the value "
                "valid or safe for this inverter.\n"
                if power > machine_max
                else ""
            )
            + "\n"
            "Continue?"
        )
        if messagebox.askyesno("Confirm write", prompt, parent=self.root):
            self._run_operation(
                lambda: self._write_worker(
                    device_id, power, timeout, machine_max
                ),
                f"Writing {power} W...",
            )

    def _write_worker(
        self,
        device_id: int,
        power: int,
        timeout: float,
        machine_max: int,
    ) -> None:
        self._ui(self.write_result_var.set, "TESTING")
        if power > machine_max:
            self._worker_log(
                "WARN",
                f"LIMIT WARNING: requested {power} W exceeds the configured "
                f"machine maximum of {machine_max} W. A successful Modbus "
                "readback only proves register storage, not valid inverter "
                "capability.",
            )
        self._worker_log(
            "INFO",
            "RD diagnostic: reading 0x04E5 before the write.",
        )
        before_value = self.worker.read_power(device_id, timeout)
        before_request = self.worker.last_request or b""
        before_response = self.worker.last_response or b""

        ack_type = self.worker.write_power(device_id, power, timeout)
        write_request = self.worker.last_request or b""
        write_response = self.worker.last_response or b""
        if ack_type == "byte_addressed":
            self._worker_log(
                "WARN",
                "A related non-standard P17 response was observed for "
                f"{power} W. The write is still unverified.",
            )
        else:
            self._worker_log("INFO", f"Write response accepted: {power} W.")

        # A value above the configured machine maximum can be echoed
        # immediately and then clamped by the inverter about 25 seconds later.
        # Keep observing long enough to distinguish temporary register storage
        # from the final retained setting.
        over_limit = power > machine_max
        if over_limit:
            delays = (0.3, 5.0, 20.0)
        elif ack_type == "byte_addressed":
            delays = (0.3, 1.0, 2.0)
        else:
            delays = (0.3,)
        readbacks: list[tuple[float, int, bytes, bytes]] = []
        for attempt, delay in enumerate(delays, start=1):
            time.sleep(delay)
            readback = self.worker.read_power(device_id, timeout)
            readbacks.append(
                (
                    delay,
                    readback,
                    self.worker.last_request or b"",
                    self.worker.last_response or b"",
                )
            )
            self._ui(self.current_power_var.set, f"{readback} W")
            if over_limit:
                if attempt < len(delays):
                    elapsed = sum(item[0] for item in readbacks)
                    self._worker_log(
                        "WARN",
                        f"Over-limit observation {attempt}/{len(delays)}: "
                        f"read {readback} W after {elapsed:.1f} s. "
                        "Continuing to watch for automatic limiting.",
                    )
                    continue

                limit_result = classify_over_limit_result(
                    power, machine_max, readback
                )
                if limit_result == "LIMIT_CLAMPED":
                    display_result = (
                        f"LIMIT CLAMPED - {power} -> {readback} W"
                    )
                    result_message = (
                        f"RESULT LIMIT_CLAMPED: requested {power} W was "
                        f"temporarily accepted, then the inverter limited it "
                        f"to the configured maximum {machine_max} W."
                    )
                elif limit_result == "LIMIT_REDUCED":
                    display_result = (
                        f"LIMIT REDUCED - {power} -> {readback} W"
                    )
                    result_message = (
                        f"RESULT LIMIT_REDUCED: requested {power} W exceeded "
                        f"the configured maximum {machine_max} W; the final "
                        f"readback was reduced to {readback} W."
                    )
                elif limit_result == "ABOVE_LIMIT_RETAINED":
                    display_result = (
                        f"ABOVE LIMIT RETAINED - {readback} W"
                    )
                    result_message = (
                        f"RESULT ABOVE_LIMIT_RETAINED: {power} W remained "
                        f"stored after the observation period even though the "
                        f"configured machine maximum is {machine_max} W."
                    )
                else:
                    display_result = (
                        f"ABOVE LIMIT UNSTABLE - final {readback} W"
                    )
                    result_message = (
                        f"RESULT ABOVE_LIMIT_UNSTABLE: requested {power} W, "
                        f"configured maximum {machine_max} W, final readback "
                        f"{readback} W."
                    )

                report = self._build_rd_report(
                    device_id=device_id,
                    power=power,
                    machine_max=machine_max,
                    timeout=timeout,
                    before_value=before_value,
                    before_request=before_request,
                    before_response=before_response,
                    ack_type=ack_type,
                    write_request=write_request,
                    write_response=write_response,
                    readbacks=readbacks,
                    verified=readback == power,
                    limit_result=limit_result,
                )
                self._ui(self._set_rd_report, report)
                self._ui(self.write_result_var.set, display_result)
                self._worker_log("WARN", result_message)
                return

            if readback == power:
                report = self._build_rd_report(
                    device_id=device_id,
                    power=power,
                    machine_max=machine_max,
                    timeout=timeout,
                    before_value=before_value,
                    before_request=before_request,
                    before_response=before_response,
                    ack_type=ack_type,
                    write_request=write_request,
                    write_response=write_response,
                    readbacks=readbacks,
                    verified=True,
                    limit_result=None,
                )
                self._ui(self._set_rd_report, report)
                if before_value == power:
                    outcome = "UNCHANGED"
                    outcome_message = (
                        f"RESULT UNCHANGED: register 0x04E5 was already "
                        f"{power} W and remained {power} W after FC16."
                    )
                    display_result = f"UNCHANGED — {power} W"
                else:
                    outcome = "CHANGED"
                    outcome_message = (
                        f"RESULT CHANGED: register 0x04E5 changed from "
                        f"{before_value} W to {power} W and was verified."
                    )
                    display_result = (
                        f"CHANGED — {before_value} → {power} W"
                    )
                self._ui(self.write_result_var.set, display_result)
                self._worker_log("INFO", outcome_message)
                self._worker_log(
                    "INFO",
                    f"Write and readback verified ({outcome}): {power} W "
                    f"(verification {attempt}/{len(delays)}).",
                )
                return
            if attempt < len(delays):
                self._worker_log(
                    "WARN",
                    f"Verification {attempt}/{len(delays)} read "
                    f"{readback} W; waiting and checking again.",
                )

        report = self._build_rd_report(
            device_id=device_id,
            power=power,
            machine_max=machine_max,
            timeout=timeout,
            before_value=before_value,
            before_request=before_request,
            before_response=before_response,
            ack_type=ack_type,
            write_request=write_request,
            write_response=write_response,
            readbacks=readbacks,
            verified=False,
            limit_result=None,
        )
        self._ui(self._set_rd_report, report)
        values = ", ".join(f"{item[1]} W" for item in readbacks)
        self._ui(
            self.write_result_var.set,
            f"FAILED — requested {power} W",
        )
        raise ModbusError(
            f"Write NOT verified for ID {device_id}: requested {power} W, "
            f"FC03 readback(s) were {values}. The inverter did not retain "
            "register 0x04E5."
        )

    def _build_rd_report(
        self,
        *,
        device_id: int,
        power: int,
        machine_max: int,
        timeout: float,
        before_value: int,
        before_request: bytes,
        before_response: bytes,
        ack_type: str,
        write_request: bytes,
        write_response: bytes,
        readbacks: list[tuple[float, int, bytes, bytes]],
        verified: bool,
        limit_result: str | None,
    ) -> str:
        expected_ack = append_crc(
            bytes((device_id, 0x10))
            + POWER_REGISTER.to_bytes(2, "big")
            + POWER_REGISTER_QUANTITY.to_bytes(2, "big")
        )
        response_address = (
            int.from_bytes(write_response[2:4], "big")
            if len(write_response) == 8
            else None
        )
        response_quantity = (
            int.from_bytes(write_response[4:6], "big")
            if len(write_response) == 8
            else None
        )
        readback_lines = []
        for index, (delay, value, request, response) in enumerate(
            readbacks, start=1
        ):
            readback_lines.extend(
                (
                    f"  Verification {index} "
                    f"(waited {delay:.1f} s since previous step):",
                    f"    TX: {hex_bytes(request)}",
                    f"    RX: {hex_bytes(response)}",
                    f"    Response CRC valid: "
                    f"{'yes' if verify_crc(response) else 'no'}",
                    f"    Decoded unsigned 32-bit value: {value} W "
                    f"(0x{value:08X})",
                )
            )

        actual_decode = "Unable to decode the FC16 response."
        if response_address is not None and response_quantity is not None:
            actual_decode = (
                f"Slave={device_id}, FC=0x10, "
                f"address=0x{response_address:04X}, "
                f"quantity=0x{response_quantity:04X}"
            )

        final_value = readbacks[-1][1] if readbacks else None
        if limit_result == "LIMIT_CLAMPED":
            result = (
                f"WARNING / LIMIT_CLAMPED: {power} W was initially accepted "
                f"but the final FC03 readback was clamped to the configured "
                f"machine maximum of {machine_max} W."
            )
        elif limit_result == "LIMIT_REDUCED":
            result = (
                f"WARNING / LIMIT_REDUCED: requested {power} W exceeded the "
                f"configured maximum {machine_max} W. The final FC03 "
                f"readback was {final_value} W."
            )
        elif limit_result == "ABOVE_LIMIT_RETAINED":
            result = (
                f"WARNING / ABOVE_LIMIT_RETAINED: FC03 still returned "
                f"{power} W after the extended observation period, although "
                f"the configured machine maximum is {machine_max} W."
            )
        elif limit_result == "ABOVE_LIMIT_UNSTABLE":
            result = (
                f"WARNING / ABOVE_LIMIT_UNSTABLE: requested {power} W, "
                f"configured maximum {machine_max} W, final FC03 readback "
                f"{final_value} W."
            )
        elif verified and before_value == power:
            result = (
                f"PASS / UNCHANGED: the pre-write value was already "
                f"{power} W. FC16 returned a response and FC03 confirmed "
                f"that the value remained {power} W."
            )
        elif verified:
            result = (
                f"PASS / CHANGED: register 0x04E5 changed from "
                f"{before_value} W to {power} W and FC03 verified the "
                "new value."
            )
        else:
            result = (
                f"FAIL: none of the FC03 readbacks matched {power} W. "
                "The requested setting was not verified or retained."
            )
        relationship = (
            "The returned address and quantity are exactly twice the requested "
            "register address and register count "
            f"(0x{POWER_REGISTER:04X} x 2 = "
            f"0x{POWER_REGISTER * 2:04X}; "
            f"{POWER_REGISTER_QUANTITY} registers x 2 = "
            f"{POWER_REGISTER_QUANTITY * 2} bytes)."
            if ack_type == "byte_addressed"
            else "The FC16 response used standard Modbus register addressing."
        )
        response_note = (
            "The response is related to the request but uses a non-standard "
            "address/count echo. It is not treated as proof of a write until "
            "FC03 readback succeeds."
            if ack_type == "byte_addressed"
            else (
                "The response is a standard FC16 acknowledgement. FC03 "
                "readback is still used to verify the retained value."
            )
        )

        return "\n".join(
            (
                "InfiniSolar P17 Modbus RTU Write Diagnostic Report",
                "=" * 55,
                f"Generated: {datetime.now().astimezone().isoformat(timespec='seconds')}",
                f"Serial port: {self.worker.port or 'unknown'}",
                f"Serial settings: {self.worker.baudrate or 'unknown'} "
                "baud, 8N1",
                f"Response timeout: {timeout:.1f} s",
                "",
                "Documented operation",
                "--------------------",
                f"Slave ID: {device_id}",
                f"Register: 0x{POWER_REGISTER:04X}",
                "Read: FC03, 2 registers",
                "Write: FC16, 2 registers / 4 data bytes",
                "Data format: unsigned 32-bit, high word first, unit W",
                f"Requested value: {power} W (0x{power:08X})",
                f"Configured machine maximum: {machine_max} W",
                f"Requested value exceeds configured maximum: "
                f"{'yes' if power > machine_max else 'no'}",
                "",
                "Pre-write FC03 read",
                "-------------------",
                f"TX: {hex_bytes(before_request)}",
                f"RX: {hex_bytes(before_response)}",
                f"Response CRC valid: "
                f"{'yes' if verify_crc(before_response) else 'no'}",
                f"Decoded value: {before_value} W (0x{before_value:08X})",
                "",
                "FC16 write",
                "----------",
                f"TX: {hex_bytes(write_request)}",
                f"Expected standard response: {hex_bytes(expected_ack)}",
                f"Actual response:            {hex_bytes(write_response)}",
                f"Actual response CRC valid: "
                f"{'yes' if verify_crc(write_response) else 'no'}",
                f"Actual response decode: {actual_decode}",
                f"Application classification: {ack_type}",
                f"Observation: {relationship}",
                response_note,
                "",
                "FC03 verification reads",
                "-----------------------",
                *readback_lines,
                "",
                "Result",
                "------",
                result,
                "",
                "Questions for inverter firmware / protocol RD",
                "---------------------------------------------",
                "1. Does this firmware support FC16 writes to register "
                "0x04E5 on this slave ID and in parallel-master mode?",
                "2. Why does FC16 return address 0x09CA and quantity 0x0004 "
                "instead of echoing 0x04E5 and 0x0002?",
                "3. Is 0x09CA/0x0004 an internal byte-addressed acknowledgement, "
                "a gateway response, or a response to another transaction?",
                "4. Is another enable/apply register, operating mode, or "
                "parallel-system command required before changing 0x04E5?",
                "5. Please provide the correct protocol/register map for this "
                "10/15 kW firmware and parallel-master role.",
            )
        )

    def _start_read(self) -> None:
        try:
            device_id, timeout = self._transaction_settings()
        except ValueError as error:
            self._show_error("Invalid setting", str(error))
            return
        if not self.worker.is_connected:
            self._show_error("Read", "Connect to the inverter first.")
            return
        self._run_operation(
            lambda: self._read_worker(device_id, timeout),
            "Reading...",
        )

    def _read_worker(self, device_id: int, timeout: float) -> None:
        power = self.worker.read_power(device_id, timeout)
        self._ui(self.current_power_var.set, f"{power} W")
        self._worker_log(
            "INFO",
            f"Read succeeded: ID={device_id}, "
            f"register=0x{POWER_REGISTER:04X}, value={power} W.",
        )

    def _start_version_read(self) -> None:
        try:
            device_id, timeout = self._transaction_settings()
        except ValueError as error:
            self._show_error("Invalid setting", str(error))
            return
        if not self.worker.is_connected:
            self._show_error("Read version", "Connect to the inverter first.")
            return
        self._run_operation(
            lambda: self._version_read_worker(device_id, timeout),
            "Reading version registers...",
        )

    def _version_read_worker(
        self, device_id: int, timeout: float
    ) -> None:
        registers = self.worker.read_registers(
            device_id,
            VERSION_REGISTER_START,
            VERSION_REGISTER_QUANTITY,
            timeout,
        )
        first, second = registers
        raw = first.to_bytes(2, "big") + second.to_bytes(2, "big")
        dotted_bytes = ".".join(str(value) for value in raw)
        ascii_text = "".join(
            chr(value) if 32 <= value <= 126 else "."
            for value in raw
        )
        display = (
            f"R1208=0x{first:04X} ({first}), "
            f"R1209=0x{second:04X} ({second})"
        )
        self._ui(self.version_var.set, display)
        self._worker_log(
            "INFO",
            f"Version registers read: ID={device_id}, "
            f"1208 (0x04B8)=0x{first:04X} ({first}), "
            f"1209 (0x04B9)=0x{second:04X} ({second}).",
        )
        self._worker_log(
            "INFO",
            f"Version raw bytes: {hex_bytes(raw)}; "
            f"byte-version candidate={dotted_bytes}; "
            f"ASCII='{ascii_text}'.",
        )

    def _start_charge_voltage_read(self) -> None:
        try:
            device_id, timeout = self._transaction_settings()
        except ValueError as error:
            self._show_error("Invalid setting", str(error))
            return
        if not self.worker.is_connected:
            self._show_error(
                "Read charging voltage",
                "Connect to the inverter first.",
            )
            return
        self._run_operation(
            lambda: self._charge_voltage_read_worker(device_id, timeout),
            "Reading charging voltages...",
        )

    def _charge_voltage_read_worker(
        self, device_id: int, timeout: float
    ) -> tuple[int, int]:
        values = self.worker.read_registers(
            device_id,
            CHARGE_CV_REGISTER,
            2,
            timeout,
        )
        cv_raw, float_raw = values
        self._ui(
            self.charge_cv_current_var.set,
            self._format_voltage(cv_raw),
        )
        self._ui(
            self.charge_float_current_var.set,
            self._format_voltage(float_raw),
        )
        self._worker_log(
            "INFO",
            f"Charging voltages read: ID={device_id}, "
            f"C.V. 0x{CHARGE_CV_REGISTER:04X}="
            f"{self._format_voltage(cv_raw)} (raw {cv_raw}), "
            f"Floating 0x{CHARGE_FLOAT_REGISTER:04X}="
            f"{self._format_voltage(float_raw)} (raw {float_raw}).",
        )
        return cv_raw, float_raw

    def _confirm_charge_voltage_write(self, target: str) -> None:
        try:
            device_id, timeout = self._transaction_settings()
            if target == "cv":
                label = "Battery constant charge voltage (C.V.)"
                address = CHARGE_CV_REGISTER
                text = self.charge_cv_set_var.get()
            elif target == "float":
                label = "Battery floating charge voltage"
                address = CHARGE_FLOAT_REGISTER
                text = self.charge_float_set_var.get()
            else:
                raise ValueError("Unknown charging-voltage target.")
            raw_value = self._parse_voltage(text)
        except ValueError as error:
            self._show_error("Invalid charging voltage", str(error))
            return
        if not self.worker.is_connected:
            self._show_error(
                "Write charging voltage",
                "Connect to the inverter first.",
            )
            return

        prompt = (
            "The following battery charging parameter will be written:\n\n"
            f"Inverter ID: {device_id}\n"
            f"Parameter: {label}\n"
            f"Register: 0x{address:04X}\n"
            f"Voltage: {self._format_voltage(raw_value)}\n"
            f"Raw register value: {raw_value}\n\n"
            "The application will first read the device limits and the other "
            "charging voltage, then write with FC16 and verify with FC03.\n\n"
            "Continue?"
        )
        if messagebox.askyesno(
            "Confirm charging-voltage write",
            prompt,
            parent=self.root,
        ):
            self._run_operation(
                lambda: self._charge_voltage_write_worker(
                    device_id,
                    target,
                    address,
                    raw_value,
                    timeout,
                ),
                f"Writing {label}...",
            )

    def _charge_voltage_write_worker(
        self,
        device_id: int,
        target: str,
        address: int,
        raw_value: int,
        timeout: float,
    ) -> None:
        current_cv, current_float = self._charge_voltage_read_worker(
            device_id, timeout
        )
        upper, lower = self.worker.read_registers(
            device_id,
            CHARGE_LIMIT_UPPER_REGISTER,
            2,
            timeout,
        )
        if lower > upper:
            raise ModbusError(
                "Device returned an invalid charging-voltage range: "
                f"lower={self._format_voltage(lower)}, "
                f"upper={self._format_voltage(upper)}."
            )
        self._worker_log(
            "INFO",
            "Device charging-voltage range: "
            f"{self._format_voltage(lower)} to "
            f"{self._format_voltage(upper)} "
            f"(registers 0x{CHARGE_LIMIT_LOWER_REGISTER:04X}/"
            f"0x{CHARGE_LIMIT_UPPER_REGISTER:04X}).",
        )
        if not lower <= raw_value <= upper:
            raise ModbusError(
                f"Write blocked: {self._format_voltage(raw_value)} is "
                "outside the device-reported charging-voltage range "
                f"{self._format_voltage(lower)} to "
                f"{self._format_voltage(upper)}."
            )
        if target == "cv" and raw_value < current_float:
            raise ModbusError(
                "Write blocked: C.V. voltage cannot be lower than the "
                f"current floating voltage "
                f"{self._format_voltage(current_float)}."
            )
        if target == "float" and raw_value > current_cv:
            raise ModbusError(
                "Write blocked: floating voltage cannot be higher than the "
                f"current C.V. voltage {self._format_voltage(current_cv)}."
            )

        before = current_cv if target == "cv" else current_float
        ack_type = self.worker.write_registers(
            device_id,
            address,
            (raw_value,),
            timeout,
        )
        self._worker_log(
            "INFO" if ack_type == "standard" else "WARN",
            f"Charging-voltage FC16 response classification: {ack_type}.",
        )
        time.sleep(0.3)
        verified_cv, verified_float = self._charge_voltage_read_worker(
            device_id, timeout
        )
        verified = verified_cv if target == "cv" else verified_float
        if verified != raw_value:
            raise ModbusError(
                f"Charging-voltage write NOT verified at 0x{address:04X}: "
                f"requested {self._format_voltage(raw_value)}, "
                f"read back {self._format_voltage(verified)}."
            )
        outcome = "UNCHANGED" if before == raw_value else "CHANGED"
        self._worker_log(
            "INFO",
            f"Charging-voltage write verified ({outcome}): "
            f"register 0x{address:04X}, "
            f"{self._format_voltage(before)} -> "
            f"{self._format_voltage(verified)}.",
        )

    @staticmethod
    def _format_voltage(raw_value: int) -> str:
        return f"{raw_value / 10:.1f} V"

    @staticmethod
    def _parse_voltage(text: str) -> int:
        try:
            value = Decimal(text.strip())
        except InvalidOperation as error:
            raise ValueError(
                "Voltage must be a number with at most one decimal place."
            ) from error
        scaled = value * 10
        if scaled != scaled.to_integral_value():
            raise ValueError(
                "Voltage supports only 0.1 V resolution."
            )
        raw_value = int(scaled)
        if not 0 <= raw_value <= 0xFFFF:
            raise ValueError(
                "Voltage must be between 0.0 V and 6553.5 V."
            )
        return raw_value

    def _transaction_settings(self) -> tuple[int, float]:
        try:
            device_id = int(self.id_var.get())
        except ValueError as error:
            raise ValueError("Inverter ID must be a whole number.") from error
        if not 1 <= device_id <= 247:
            raise ValueError("Inverter ID must be between 1 and 247.")
        try:
            timeout = float(self.timeout_var.get())
        except ValueError as error:
            raise ValueError("Invalid timeout.") from error
        if timeout not in TIMEOUTS:
            raise ValueError("Select one of the available timeout values.")
        return device_id, timeout

    def _machine_max_power(self) -> int:
        try:
            value = int(self.machine_max_var.get().strip())
        except ValueError as error:
            raise ValueError(
                "Machine maximum must be a whole number of watts."
            ) from error
        if not 1 <= value <= 0xFFFFFFFF:
            raise ValueError(
                "Machine maximum must be between 1 and 4294967295 W."
            )
        return value

    def _run_operation(self, function, status_text: str) -> None:
        if not self.operation_lock.acquire(blocking=False):
            self._append_log("WARN", "Another operation is already running.")
            return
        self._set_busy(True, status_text)

        def runner() -> None:
            try:
                function()
            except Exception as error:
                self._worker_log("ERROR", str(error))
                self._ui(self._show_error, "Operation failed", str(error))
            finally:
                self.operation_lock.release()
                self._ui(self._set_busy, False, "")

        threading.Thread(target=runner, daemon=True).start()

    def _set_busy(self, busy: bool, status_text: str) -> None:
        state = "disabled" if busy else "normal"
        for widget in self.control_widgets:
            widget.configure(state=state)
        if busy:
            self.connect_button.configure(state="disabled")
            self.disconnect_button.configure(state="disabled")
            self.scan_button.configure(state="disabled")
            self.status_var.set(status_text)
            self.status_label.configure(foreground="#8a5a00")
        else:
            if self.worker.is_connected:
                self.status_var.set(f"Connected: {self.worker.port}")
                self.status_label.configure(foreground="#087f23")
            else:
                self.status_var.set("Disconnected")
                self.status_label.configure(foreground="#b00020")
            self._refresh_connection_controls()

    def _set_connection_status(
        self, connected: bool, port: str
    ) -> None:
        if connected:
            self.status_var.set(f"Connected: {port}")
            self.status_label.configure(foreground="#087f23")
        else:
            self.status_var.set("Disconnected")
            self.status_label.configure(foreground="#b00020")
            self.current_power_var.set("-- W")
            self.write_result_var.set("No write yet")
            self.version_var.set("Not read")
            self.charge_cv_current_var.set("-- V")
            self.charge_float_current_var.set("-- V")
        self._refresh_connection_controls()

    def _refresh_connection_controls(self) -> None:
        connected = self.worker.is_connected
        self.connect_button.configure(
            state="disabled" if connected else "normal"
        )
        self.disconnect_button.configure(
            state="normal" if connected else "disabled"
        )
        self.scan_button.configure(
            state="disabled" if connected else "normal"
        )
        self.com_box.configure(
            state="disabled" if connected else "readonly"
        )
        self.baud_box.configure(
            state="disabled" if connected else "readonly"
        )
        for widget in self.control_widgets:
            widget.configure(state="normal")
        self.timeout_box.configure(state="readonly")
        for widget in self.action_widgets:
            widget.configure(state="normal" if connected else "disabled")

    def _show_error(self, title: str, message: str) -> None:
        messagebox.showerror(title, message, parent=self.root)

    def _on_close(self) -> None:
        try:
            self.worker.disconnect()
        finally:
            self.root.destroy()
