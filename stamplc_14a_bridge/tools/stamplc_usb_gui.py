"""Windows desktop configuration GUI for 14a Bridge."""

from __future__ import annotations

import queue
import re
import threading
import time
import tkinter as tk
from dataclasses import dataclass
from datetime import datetime
from tkinter import messagebox, ttk

import serial
import serial.tools.list_ports


USB_BAUD = 115200
ID_LINE = re.compile(
    r"^(?P<id>[1-6])\s+(?P<enabled>yes|no)\s+"
    r"(?P<maximum>\d+)\s+\d+\s+\d+\s+\d+\s+\d+\s+"
    r"(?P<requested>\d+)\s+(?P<readback>\d+)\s+(?P<healthy>yes|no)$"
)
RSE_LINE = re.compile(
    r"^RSE DI mask:\s*(?P<mask>0x[0-9A-Fa-f]+)\s+level:\s*(?P<level>\d+%|INVALID)$"
)
MODE_LINE = re.compile(
    r"^Mode:\s*(?P<mode>DRY-RUN|LIVE)\s+RS485:\s*(?P<baud>\d+) baud\s+"
    r"register:\s*(?P<register>0x[0-9A-Fa-f]+)\s+quantity:\s*(?P<quantity>[12])$"
)
PROBE_LINE = re.compile(
    r"^PROBE ID=(?P<id>[1-6]) REGISTER=(?P<register>0x[0-9A-Fa-f]+) "
    r"VALUE=(?P<readback>\d+) STATUS=(?P<status>OK|RETRY|ERROR) DETAIL=(?P<detail>.*)$"
)


@dataclass(frozen=True)
class ParsedLine:
    kind: str
    values: dict[str, str]


def parse_status_line(line: str) -> ParsedLine | None:
    """Parse one stable, machine-useful line from the firmware `show` output."""
    stripped = line.strip()
    for kind, pattern in (
        ("inverter", ID_LINE), ("rse", RSE_LINE), ("mode", MODE_LINE),
        ("probe", PROBE_LINE),
    ):
        match = pattern.match(stripped)
        if match:
            return ParsedLine(kind, match.groupdict())
    return None


class SerialLink:
    def __init__(self, event_queue: queue.Queue[tuple[str, str]]) -> None:
        self._events = event_queue
        self._port: serial.Serial | None = None
        self._write_lock = threading.Lock()
        self._stop = threading.Event()
        self._reader: threading.Thread | None = None

    @property
    def connected(self) -> bool:
        return self._port is not None and self._port.is_open

    def connect(self, port_name: str) -> None:
        self.disconnect()
        self._port = serial.Serial(
            port=port_name,
            baudrate=USB_BAUD,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,
            write_timeout=1.0,
        )
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def disconnect(self) -> None:
        self._stop.set()
        port = self._port
        self._port = None
        if port is not None:
            try:
                if port.is_open:
                    port.close()
            except serial.SerialException:
                pass

    def send(self, command: str) -> None:
        if not self.connected or self._port is None:
            raise serial.SerialException("StampPLC is not connected.")
        payload = (command.strip() + "\n").encode("ascii")
        with self._write_lock:
            self._port.write(payload)
            self._port.flush()

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            port = self._port
            if port is None:
                return
            try:
                raw = port.readline()
                if raw:
                    self._events.put(("line", raw.decode("utf-8", errors="replace").rstrip()))
            except (serial.SerialException, OSError) as exc:
                if not self._stop.is_set():
                    self._events.put(("error", str(exc)))
                return


class StampPlcConfigurator:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("14a Bridge - Engineering Console V1.0.4")
        self.root.geometry("980x760")
        self.root.minsize(840, 650)

        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self.link = SerialLink(self.events)
        self.port_var = tk.StringVar()
        self.connection_var = tk.StringVar(value="Disconnected")
        self.rse_var = tk.StringVar(value="RSE: --")
        self.mode_var = tk.StringVar(value="Mode: --")
        self.output_var = tk.StringVar(value="Output: --")
        self.baud_var = tk.StringVar(value="19200")
        self.register_var = tk.StringVar(value="0x04E5")
        self.enabled_vars = [tk.BooleanVar(value=False) for _ in range(6)]
        self.maximum_vars = [tk.StringVar(value="10000") for _ in range(6)]
        self.request_vars = [tk.StringVar(value="--") for _ in range(6)]
        self.readback_vars = [tk.StringVar(value="--") for _ in range(6)]
        self.health_vars = [tk.StringVar(value="--") for _ in range(6)]
        self.is_dry_run: bool | None = None

        self._build_ui()
        self._scan_ports()
        self.root.after(50, self._poll_events)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=12)
        outer.pack(fill="both", expand=True)

        connection = ttk.LabelFrame(outer, text="USB Connection", padding=10)
        connection.pack(fill="x", pady=(0, 8))
        self.port_box = ttk.Combobox(
            connection, textvariable=self.port_var, state="readonly", width=25
        )
        self.port_box.grid(row=0, column=0, padx=4)
        ttk.Button(connection, text="Scan", command=self._scan_ports).grid(row=0, column=1, padx=4)
        self.connect_button = ttk.Button(connection, text="Connect", command=self._toggle_connection)
        self.connect_button.grid(row=0, column=2, padx=4)
        ttk.Label(connection, textvariable=self.connection_var).grid(row=0, column=3, padx=12, sticky="w")
        connection.columnconfigure(3, weight=1)

        status = ttk.LabelFrame(outer, text="Live Status", padding=10)
        status.pack(fill="x", pady=(0, 8))
        ttk.Label(status, textvariable=self.rse_var, font=("Segoe UI", 11, "bold")).grid(row=0, column=0, padx=8)
        ttk.Label(status, textvariable=self.mode_var, font=("Segoe UI", 11, "bold")).grid(row=0, column=1, padx=8)
        ttk.Label(status, textvariable=self.output_var).grid(row=0, column=2, padx=8)
        ttk.Button(status, text="Read controller settings", command=lambda: self._send("show")).grid(row=0, column=3, padx=8)
        ttk.Button(status, text="Probe enabled IDs (FC03)", command=lambda: self._send("probe all")).grid(row=0, column=4, padx=8)

        devices = ttk.LabelFrame(outer, text="Inverters", padding=10)
        devices.pack(fill="x", pady=(0, 8))
        headers = ("ID", "Control enabled", "Maximum PV power (W)", "Last target", "Readback", "Status")
        for column, label in enumerate(headers):
            ttk.Label(devices, text=label, font=("Segoe UI", 9, "bold")).grid(
                row=0, column=column, padx=6, pady=3
            )
        for i in range(6):
            ttk.Label(devices, text=str(i + 1)).grid(row=i + 1, column=0, padx=6)
            ttk.Checkbutton(devices, variable=self.enabled_vars[i]).grid(row=i + 1, column=1)
            ttk.Entry(devices, textvariable=self.maximum_vars[i], width=18).grid(
                row=i + 1, column=2, padx=6, pady=2
            )
            ttk.Label(devices, textvariable=self.request_vars[i], width=12).grid(row=i + 1, column=3)
            ttk.Label(devices, textvariable=self.readback_vars[i], width=12).grid(row=i + 1, column=4)
            ttk.Label(devices, textvariable=self.health_vars[i], width=10).grid(row=i + 1, column=5)

        settings = ttk.LabelFrame(outer, text="Device Settings", padding=10)
        settings.pack(fill="x", pady=(0, 8))
        ttk.Label(settings, text="RS485 baud").grid(row=0, column=0, padx=4)
        ttk.Entry(settings, textvariable=self.baud_var, width=12).grid(row=0, column=1, padx=4)
        ttk.Label(settings, text="Power register").grid(row=0, column=2, padx=(15, 4))
        ttk.Entry(settings, textvariable=self.register_var, width=12).grid(row=0, column=3, padx=4)
        ttk.Button(settings, text="Save all settings", command=self._save_all).grid(row=0, column=4, padx=15)
        ttk.Button(settings, text="Sync RTC from PC", command=self._sync_time).grid(row=0, column=5, padx=4)

        actions = ttk.LabelFrame(outer, text="Commissioning", padding=10)
        actions.pack(fill="x", pady=(0, 8))
        ttk.Button(actions, text="Enable Dry-run", command=lambda: self._send("dryrun on")).grid(row=0, column=0, padx=4)
        ttk.Button(actions, text="Enable LIVE control", command=self._enable_live).grid(row=0, column=1, padx=4)
        for column, level in enumerate((100, 60, 30, 0), start=2):
            ttk.Button(
                actions, text=f"Test {level}%", command=lambda value=level: self._test_level(value)
            ).grid(row=0, column=column, padx=4)
        ttk.Button(actions, text="Apply current RSE", command=self._apply_current).grid(row=0, column=6, padx=4)

        log_frame = ttk.LabelFrame(outer, text="USB Device Log", padding=8)
        log_frame.pack(fill="both", expand=True)
        self.log = tk.Text(log_frame, height=14, wrap="none", font=("Consolas", 9), state="disabled")
        yscroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        xscroll = ttk.Scrollbar(log_frame, orient="horizontal", command=self.log.xview)
        self.log.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.log.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)

    def _scan_ports(self) -> None:
        ports = list(serial.tools.list_ports.comports())
        names = [port.device for port in ports]
        self.port_box["values"] = names
        if self.port_var.get() not in names:
            preferred = next(
                (port.device for port in ports if "USB" in port.description.upper()),
                names[0] if names else "",
            )
            self.port_var.set(preferred)

    def _toggle_connection(self) -> None:
        if self.link.connected:
            self.link.disconnect()
            self.connect_button.configure(text="Connect")
            self.connection_var.set("Disconnected")
            return
        port = self.port_var.get()
        if not port:
            messagebox.showwarning("No COM port", "Connect the StampPLC by USB and scan again.")
            return
        try:
            self.link.connect(port)
        except (serial.SerialException, OSError) as exc:
            messagebox.showerror("Connection failed", str(exc))
            return
        self.connect_button.configure(text="Disconnect")
        self.connection_var.set(f"Connected to {port} at {USB_BAUD}")
        self._append_log(f"[PC] Connected to {port}")
        self.root.after(250, lambda: self._send("show"))

    def _send(self, command: str) -> bool:
        try:
            self.link.send(command)
        except (serial.SerialException, OSError) as exc:
            messagebox.showerror("USB error", str(exc))
            return False
        self._append_log(f"[PC] > {command}")
        return True

    def _save_all(self) -> None:
        if not self.link.connected:
            messagebox.showwarning("Not connected", "Connect to the StampPLC first.")
            return
        maxima: list[int] = []
        try:
            for text in (variable.get().strip() for variable in self.maximum_vars):
                value = int(text, 0)
                if not 1 <= value <= 0xFFFFFFFF:
                    raise ValueError
                maxima.append(value)
            baud = int(self.baud_var.get().strip(), 0)
            if not 1200 <= baud <= 1_000_000:
                raise ValueError
            register = int(self.register_var.get().strip(), 0)
            if not 0 <= register <= 0xFFFF:
                raise ValueError
        except ValueError:
            messagebox.showerror(
                "Invalid setting",
                "Power must be 1..4294967295 W, baud 1200..1000000, and register 0..0xFFFF.",
            )
            return

        commands: list[str] = []
        for i in range(6):
            commands.append(f"id {i + 1} {'on' if self.enabled_vars[i].get() else 'off'}")
            commands.append(f"max {i + 1} {maxima[i]}")
        commands.extend((f"baud {baud}", f"reg 0x{register:04X}", "show"))
        for index, command in enumerate(commands):
            self.root.after(index * 80, lambda value=command: self._send(value))
        self.connection_var.set("Saving configuration...")
        self.root.after(len(commands) * 80 + 300, lambda: self.connection_var.set("Configuration sent"))

    def _sync_time(self) -> None:
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self._send(f"time {now}")

    def _enable_live(self) -> None:
        if messagebox.askyesno(
            "Enable live control",
            "This enables real Modbus writes to every checked inverter ID. Continue?",
            icon="warning",
        ):
            self._send("dryrun off CONFIRM")
            self.root.after(250, lambda: self._send("show"))

    def _test_level(self, level: int) -> None:
        command = f"test {level}"
        if self.is_dry_run is False:
            if not messagebox.askyesno(
                "Live inverter test",
                f"Write the calculated {level}% limit to all enabled inverter IDs?",
                icon="warning",
            ):
                return
            command += " CONFIRM"
        self._send(command)
        self.root.after(400, lambda: self._send("show"))

    def _apply_current(self) -> None:
        command = "apply"
        if self.is_dry_run is False:
            if not messagebox.askyesno(
                "Apply current RSE state",
                "Write the level selected by the current RSE contacts to all enabled inverter IDs?",
                icon="warning",
            ):
                return
            command += " CONFIRM"
        self._send(command)
        self.root.after(400, lambda: self._send("show"))

    def _poll_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "line":
                    self._handle_device_line(payload)
                else:
                    self._append_log(f"[USB ERROR] {payload}")
                    self.connection_var.set("USB connection lost")
                    self.link.disconnect()
                    self.connect_button.configure(text="Connect")
        except queue.Empty:
            pass
        self.root.after(50, self._poll_events)

    def _handle_device_line(self, line: str) -> None:
        self._append_log(line)
        parsed = parse_status_line(line)
        if parsed is None:
            if line.startswith("Last result:"):
                self.output_var.set(line)
            return
        values = parsed.values
        if parsed.kind == "rse":
            self.rse_var.set(f"RSE: {values['level']} ({values['mask']})")
        elif parsed.kind == "mode":
            self.is_dry_run = values["mode"] == "DRY-RUN"
            self.mode_var.set(f"Mode: {values['mode']}")
            self.baud_var.set(values["baud"])
            self.register_var.set(values["register"].upper())
        elif parsed.kind == "inverter":
            index = int(values["id"]) - 1
            self.enabled_vars[index].set(values["enabled"] == "yes")
            self.maximum_vars[index].set(values["maximum"])
            self.request_vars[index].set(values["requested"] + " W")
            self.readback_vars[index].set(values["readback"] + " W")
            self.health_vars[index].set("OK" if values["healthy"] == "yes" else "CHECK")
        elif parsed.kind == "probe":
            index = int(values["id"]) - 1
            self.readback_vars[index].set(values["readback"] + " W")
            self.health_vars[index].set(
                "FC03 OK" if values["status"] == "OK" else values["status"]
            )
            self.output_var.set(
                f"ID {values['id']}: {values['status']} - {values['detail']}"
            )

    def _append_log(self, line: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _close(self) -> None:
        self.link.disconnect()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    StampPlcConfigurator(root)
    root.mainloop()


if __name__ == "__main__":
    main()
