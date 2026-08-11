# 14A Bridge Quick Installation and Commissioning Guide

> Applies to SmartPLC firmware `V1.0.4` and Windows USB Configurator `V1.0.4`.

Complete these seven steps in order to send the RSE `100% / 60% / 30% / 0%` commands safely to up to six inverters.

> [!CAUTION]
> Never connect a 230 V RSE output directly to StampPLC. Use a correctly rated interface relay or isolation module. StampPLC digital inputs accept only 5-36 V DC.

## 1. Wire the system

![System wiring](user_manual_assets/system_wiring.png)

| Source | StampPLC terminal | Function |
|---|---|---|
| +12 V | VIN | Main power positive |
| 12 V 0 V | GND | Main power return |
| 12 V 0 V | COM | Digital-input common reference |
| +12 V | RSE relay common | Supplies +12 V through the selected relay contact |
| K1 / K2 / K3 / K4 | IN1 / IN2 / IN3 / IN4 | 100% / 60% / 30% / 0% |

RS485: connect `A -> A/D+` and `B -> B/D-`. Do not connect the PWR485 `VIN` terminal to the inverter.

**Success:** StampPLC starts and displays `V1.0.4`.

## 2. Connect by USB

1. Connect a USB Type-C data cable.
2. Click **Scan**.
3. Select the new COM port.
4. Click **Connect**.
5. Click **Read SmartPLC settings**.

**Success:** The GUI shows the detected `[SMARTPLC] COMx V1.0.4` entry, then `CONNECTED COMx` after connection.

## 3. Scan inverter IDs

1. Power the inverters and assign a unique Modbus ID to each one.
2. Open **COMMISSIONING**.
3. Click **Scan all IDs**.

**Success:** Connected IDs show `FOUND`. This is an FC03 read-only scan and does not write to an inverter.

> [!NOTE]
> The scan reads the current value of register `0x04E5`. It is not guaranteed to be the inverter nameplate maximum power.

## 4. Configure inverter power

1. Return to **SETTINGS**.
2. Enable only the IDs that are physically installed.
3. Enter `10000` for a 10 kW inverter or `15000` for a 15 kW inverter.
4. Keep `19200 baud` and register `0x04E5` unless the inverter documentation specifies different values.
5. Click **Save inverter settings**.

**Success:** Status shows `OK`, `PENDING`, or `LIMITED`. If a 15 kW inverter rejects 20 kW, the GUI automatically corrects the setting to 15,000 W.

## 5. Test the RSE inputs

1. Confirm that Mode is `DRY-RUN`.
2. Close K1, K2, K3, and K4 in sequence. Only one contact may be closed at a time.
3. Run `100% test`, `60% test`, `30% test`, and `0% test`.

| Rated power | 100% | 60% | 30% | 0% |
|---:|---:|---:|---:|---:|
| 15 kW | 15.0 kW | 9.0 kW | 4.5 kW | 0 kW |
| 10 kW | 10.0 kW | 6.0 kW | 3.0 kW | 0 kW |

**Success:** The RSE level and calculated values are correct, with no `INVALID` state.

## 6. Enable LIVE control

1. Set the physical RSE to `100%` first.
2. Click **Enable LIVE** and confirm the warning.
3. Wait for the GUI to clear the temporary test display and read the physical RSE.
4. Test `100% -> 60% -> 30% -> 0% -> 100%`.

**Success:** Mode shows `LIVE`. The dashed line is the RSE target; the liquid level and center kW value are the inverter readback.

> [!NOTE]
> The controller does not repeatedly write when the RSE state is unchanged. Background health checks are FC03 read-only operations. A failed write is retried no more than three times.

## 7. Configure Wi-Fi and OTA (optional)

1. Click **Refresh PC Wi-Fi** and select a 2.4 GHz SSID.
2. Enter the password and click **Save connect**.
3. Select **Enable automatic OTA** if automatic firmware updates are required.

**Success:** Wi-Fi shows `Connected` and automatic updates show `AUTO OTA: ON`.

The controller checks about 60 seconds after boot, about 5 seconds after automatic OTA is enabled, and then once every 24 hours. Installation starts only when Wi-Fi is connected, the RSE state is valid, and Modbus is idle.

## Troubleshooting

| Symptom | Check in this order |
|---|---|
| COM port not found | Change the USB data cable -> change USB port -> restart the GUI -> click Scan |
| All IDs show NO_RESPONSE | Inverter power -> baud rate -> Modbus ID -> RS485 A/B polarity |
| One inverter occasionally shows RETRY | Wait for the next poll -> check cable, shield, and termination -> remove a second Modbus master |
| Full red ERROR tile | Event log -> A/B -> ID -> baud -> `0x04E5` -> inverter power |
| RSE INVALID 0x00 | RSE common to +12 V -> COM to 0 V -> at least one contact closed |
| Multi-contact INVALID | Ensure only one of K1-K4 can close at a time |
| Maximum power is reduced automatically | The inverter rejected the higher value; use the corrected verified limit |
| AUTO OTA remains OFF | Connect -> Read SmartPLC settings -> enable OTA again -> wait for `AUTO OTA: ON` |
| USB flash failed | Select the COM port again; if required, hold BOOT until the red LED lights, release it, and retry |

## Final commissioning checklist

- Only physically installed Modbus IDs are enabled.
- Each maximum power value matches the inverter rating.
- The four RSE inputs map to 100%, 60%, 30%, and 0%.
- Every enabled ID completes an FC16 write and FC03 readback in LIVE mode.
- The system recovers from 0% or 30% back to 100%.
- An RS485 disconnection is detected and automatically clears after communication recovers.
- Automatic OTA shows ON when required, or remains OFF when not used.

Official StampPLC interface information: [M5Stack StampPLC documentation](https://docs.m5stack.com/en/core/StamPLC)
