# InfiniSolar IP21 RS485 Power Control

Windows Tkinter utility for reading and setting the P17 feed-in power limit over
Modbus RTU.

The protocol PDF identifies register `0x04E5` (decimal 1253) as a two-register,
read/write value in watts. This application therefore always reads it with FC03
and writes it with FC16 as a 32-bit unsigned, high-word-first value.

## Install and run

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python main.py
```

Connect a USB-RS485 adapter, select its COM port and the inverter ID, then
connect. Every write requires confirmation and is followed by an FC03 readback.

Successful writes are classified explicitly:

- **UNCHANGED**: the requested value matched the pre-write value and remained
  the same after FC16.
- **CHANGED**: the requested value differed from the pre-write value and the
  new value was confirmed by FC03.

An unverified write is shown as **FAILED**. The classification appears in the
GUI, communication log, and RD report.

Set **Machine max W** to the rated active-power limit of the inverter under
test. A request above this value is not reported as a normal successful write,
even if the first FC03 readback echoes it. The application observes the value
for about 25 seconds and classifies the final behavior as:

- **LIMIT_CLAMPED**: the inverter returns to the configured maximum.
- **LIMIT_REDUCED**: the inverter reduces the value to another value at or
  below the configured maximum.
- **ABOVE_LIMIT_RETAINED**: the over-limit value is still present after the
  observation period.
- **ABOVE_LIMIT_UNSTABLE**: the final value differs but remains above the
  configured maximum.

Each write now starts with a baseline FC03 read and produces an English
manufacturer/RD diagnostic report. After the test, use **View RD Report**,
**Copy RD Report**, or **Save RD Report** below the communication log. The
report includes the exact TX/RX frames, decoded fields, CRC status, expected
standard FC16 response, delayed FC03 verification reads, final pass/fail result,
and protocol questions for firmware RD.

The **Read Version (1208 / 1209)** button reads decimal registers 1208 and
1209 (`0x04B8` and `0x04B9`) with FC03. The GUI logs both 16-bit raw values,
their four bytes, a dotted-byte version candidate, and printable ASCII so the
firmware encoding can be identified without assuming a format.

## Battery charging voltages

The P17 battery parameters are available as two independent controls:

- **Constant/C.V. voltage**: register `0x026F` (623), one register, 0.1 V.
- **Floating voltage**: register `0x0270` (624), one register, 0.1 V.

**Read Both** reads the adjacent values with FC03. **Write C.V.** and
**Write Floating** each write only their own single register using FC16
(quantity 1, byte count 2), then read both values back for verification.

Before writing, the application reads the device-reported upper and lower
charging-voltage limits from `0x0107` and `0x0108`. It blocks values outside
that range, a C.V. voltage below the current floating voltage, and a floating
voltage above the current C.V. voltage.

## Shared-bus behavior

The receive buffer supports partial, concatenated, and multiple RTU frames. A
CRC-valid frame that does not match the active transaction is shown as `BUS` and
does not end the wait. The active transaction fails only when its timeout
expires without a matching response.

The observed parallel master returns a non-standard FC16 acknowledgement using
an internal byte address and byte count (`0x04E5 / 2 registers` becomes
`0x09CA / 4 bytes`). The transport recognizes this exact mathematical mapping
as a P17 compatibility acknowledgement, logs a warning, and still requires the
normal FC03 readback before reporting verification success. Other unrelated
FC16 frames remain classified as `BUS`.

## Tests

```powershell
python -m unittest -v
```

For a hardware read-only check:

```powershell
python hardware_acceptance_test.py COM12 --device-id 3
```

The physical write sequence is intentionally gated behind an explicit flag:

```powershell
python hardware_acceptance_test.py COM12 --device-id 3 --write-sequence
```

This writes and verifies 0, 5000, 10000, and 15000 W in order, stopping
immediately if any readback differs from the requested value.
