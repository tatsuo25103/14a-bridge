# 14a Bridge

## V1.0.1 release

For a first-time Windows installation, download and run
`14a_Bridge_Setup_V1.0.1.exe` from the GitHub release. The installer
uses the MES icon, installs the USB Configurator, the current StampPLC
firmware, and a self-contained ESP32-S3 flasher. No Python, PlatformIO, or
development tools are required on the customer's computer.

To program a new StampPLC, or upgrade V1.0.0 to the OTA partition layout:
connect it by USB, start the configurator, select its COM port, open
**Settings**, and click **USB flash V1.0.1**. This writes the controller
firmware and OTA partition table; it does not write any inverter over RS485.
The NVS addresses are unchanged so existing inverter settings are retained.

Firmware for using an M5Stack StampPLC between a ripple-control receiver
(`Rundsteuerempfänger`, RSE) and up to six P17/InfiniSolar inverters.

The RSE performs the mains ripple decoding. The StampPLC reads four RSE relay
contacts and translates the selected level to an individual watt limit for each
enabled inverter:

| StampPLC input | RSE level | Calculation for each inverter |
|---|---:|---|
| DI1 | 100% | `maximum PV W × 1.00` |
| DI2 | 60% | `maximum PV W × 0.60` |
| DI3 | 30% | `maximum PV W × 0.30` |
| DI4 | 0% | `0 W` |

Exactly one input must be active. An all-open state or multiple simultaneous
inputs is invalid: the firmware keeps the last inverter settings, logs the
invalid mask, displays an alarm, and sounds the buzzer.

## 12 V supply and RSE wiring

Use an isolated SELV/PELV 230 VAC to 12 V DC DIN-rail supply. A 1 A supply is
enough for the StampPLC itself; use 2-2.5 A if it also supplies interface relays
or other equipment.

```text
12 V power supply
  +12 V  ───────────── StampPLC VIN+
   0 V   ───────────── StampPLC VIN-

  +12 V  ───────────── RSE relay common
  RSE K1 (100%) ────── StampPLC DI1
  RSE K2 ( 60%) ────── StampPLC DI2
  RSE K3 ( 30%) ────── StampPLC DI3
  RSE K4 (  0%) ────── StampPLC DI4
   0 V   ───────────── StampPLC EXCOM_COM
```

This wiring is only for **potential-free RSE contacts**. Never connect a
switched 230 V RSE output directly to the StampPLC 5-36 V DC inputs. Use a
properly rated interface relay or isolation module when the RSE circuit uses
230 V.

The PWR-485 power pin is connected directly to StampPLC VIN and therefore also
carries 12 V. Normally connect only RS485 A/B to an inverter; do not connect
the 12 V pin unless the inverter documentation explicitly requires it.

## Configure inverter IDs 1-6

The compile-time table is in
[`include/DeviceDefaults.h`](include/DeviceDefaults.h):

```cpp
constexpr InverterDefault INVERTER_DEFAULTS[6] = {
    // enabled, maximum PV generation power in watts
    {true,  15000},  // ID 1
    {true,  15000},  // ID 2
    {true,  10000},  // ID 3
    {false, 10000},  // ID 4 disabled
    {false, 10000},  // ID 5 disabled
    {false, 10000},  // ID 6 disabled
};
```

Set `enabled` to `false` for an unused ID. This has the same effect as
commenting that inverter out while keeping the fixed ID-to-array relationship.

After first boot, the same values can be changed without recompiling through
the USB configuration GUI or USB serial commands. Saved NVS configuration
takes priority over the compiled defaults. Send `reset CONFIRM` over USB after
changing the table on an already commissioned controller.

Example for ID 3 with a 10,000 W maximum:

| RSE level | FC16 value written to ID 3 |
|---:|---:|
| 100% | 10000 W |
| 60% | 6000 W |
| 30% | 3000 W |
| 0% | 0 W |

## Implemented behavior

- Four debounced, optically isolated RSE inputs with one-hot validation.
- IDs 1-6, individually enabled and individually assigned maximum PV power.
- Integer percentage calculation with nearest-watt rounding.
- Sequential FC16 write to each enabled ID followed by FC03 readback.
- A failed control transaction retries the complete FC16 + FC03 sequence up
  to three times with increasing backoff (not merely three FC03 reads). IDs are
  separated by 500 ms. Retries are bounded; a failed transaction is reported
  and is never retried automatically in the background.
- Communication health and control conformance are tracked separately: a
  responsive inverter with a limit that does not match RES is reported as a
  write/control fault, not as an RS485-offline fault.
- Read-only FC03 probe of every enabled inverter during startup, before the
  saved RSE command is evaluated or applied.
- Periodic readback is FC03-only. It detects an externally changed inverter
  limit, but never writes it back automatically; only a new RSE transition or
  an explicit, confirmed USB command can write an inverter.
- Round-robin FC03 refresh in both DRY and LIVE modes, even while the RSE input
  is invalid. Only one enabled ID is polled every two seconds. One successful
  read immediately refreshes the displayed value and clears a fault; a full
  red ERROR tile requires three consecutive failed polls. The first two misses
  retain the last good value and use an amber warning border.
- microSD CSV fields: time, raw RSE mask, percentage, inverter ID, configured
  maximum, requested watts, readback watts, and result.
- USB CDC configuration commands and a matching Windows desktop GUI.
- Color LCD dashboard that automatically divides into 1-6 inverter tiles.
  Every tile fills vertically to the active RSE percentage and shows its ID,
  percentage, and calculated target in kW. The header shows DRY/LIVE, while
  the footer shows RS485 health and SD status. Rendering uses an off-screen
  canvas to avoid flicker. The 240x135 landscape layout uses 1x1, 2x1, 3x1,
  2x2, or 3x2 grids for 1, 2, 3, 4, or 5-6 enabled inverters respectively.
  A single header shows the global RSE percentage. Each tile is a tank-style
  gauge: cyan fill height represents the inverter FC03 value, and an amber
  dashed horizontal line represents the RES target. A compact `R... I...` kW
  line provides the exact values. This remains readable in the six-inverter
  3x2 layout. Any failed inverter uses a full red tile.
  Liquid level, RES setpoint, value counters, color transition, wave surface,
  and bubbles animate at approximately 30 FPS without changing Modbus timing.
- Onboard RTC time, RGB alarm, and buzzer.
- Safe dry-run default.
- Optional Wi-Fi station mode used only for time synchronization and GitHub
  Release OTA. No access point or inbound network service is opened.
- Two 3 MB application slots permit future firmware updates without USB.
  Firmware is downloaded over certificate-validated HTTPS and accepted only
  when its SHA-256 matches the release manifest.
- Automatic OTA is off by default. When enabled in the GUI, the controller
  checks after startup and every 24 hours. Installation starts only while the
  RSE state is valid and Modbus control is idle.

## Build and flash

```powershell
cd stamplc_14a_bridge
..\.venv\Scripts\platformio.exe run
..\.venv\Scripts\platformio.exe run -t upload
..\.venv\Scripts\platformio.exe device monitor
```

Insert a FAT32-formatted microSD card. Connect the StampPLC USB data port to a
Windows computer. The firmware appears as a USB serial COM port at 115200 baud.

## Windows USB configuration GUI

For customers, run the standalone application (no Python installation needed):

```text
release\14a_Bridge.exe
```

The customer GUI is a native Windows Forms application and requires only the
standard .NET Framework included with Windows 10/11.

For development, the equivalent Python source version can be run with:

```powershell
.\.venv\Scripts\python.exe .\stamplc_14a_bridge\tools\stamplc_usb_gui.py
```

The GUI is divided into **Settings** and **Commissioning** tabs. It can:

- scan and connect to the StampPLC COM port;
- read the current RSE state and saved configuration;
- perform a read-only FC03 probe of all enabled inverter IDs;
- enable/disable inverter IDs 1-6;
- set each inverter's maximum PV power;
- configure RS485 baud and the power-limit register;
- synchronize the StampPLC RTC from the PC;
- install the bundled V1.0.1 firmware over USB;
- save Wi-Fi credentials and show connection/IP/RSSI state;
- check or install GitHub OTA releases and enable/disable automatic OTA;
- switch between dry-run and live control with confirmation;
- test 100%, 60%, 30%, and 0%;
- reapply the level currently selected by the RSE;
- display the live USB device/event log.

The GUI uses the text command interface, so a standard serial terminal can be
used as a fallback. Enter `help` to list commands and `show` to read all
settings. Use `probe all` to read register `0x04E5` from every enabled ID, or
`probe 3` to read only ID 3. Probe commands never write an inverter value.
Successful setting commands are saved immediately to NVS.

## OTA release assets

Every OTA-capable GitHub release must contain these two assets:

- `14a_bridge_firmware.bin`
- `ota_manifest.json`

The manifest contains the semantic version, the version-specific release URL,
and the lowercase SHA-256 of the firmware. V1.0.0 must first be upgraded once
through the GUI's USB flash button because it has no Wi-Fi/OTA update client;
V1.0.1 also installs the larger 3 MB-per-slot partition layout used by future
releases.

## Commissioning sequence

1. Keep dry-run enabled and operate all four RSE states.
2. Confirm that DI1/DI2/DI3/DI4 decode as 100/60/30/0% and that no all-open
   transition occurs during normal RSE switching.
3. Confirm the inverter ID list and maximum PV power values.
4. Verify that register `0x04E5`, two registers, high-word-first, is correct for
   every connected inverter firmware version.
5. Disconnect other Modbus masters or coordinate access to prevent RS485
   collisions.
6. Use the GUI manual test buttons under supervision, then confirm FC03 readback
   and actual inverter output.
7. Only then disable dry-run and test physical RSE transitions.
8. Test power loss, RSE invalid states, RS485 disconnection, SD removal/full
   card, and restoration to 100%.

The 100/60/30/0 input mapping must match the contact table supplied by the
responsible grid operator. Change the mapping in `decodePercent()` if its RSE
contact assignment differs.
