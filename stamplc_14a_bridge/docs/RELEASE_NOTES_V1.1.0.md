# 14a Bridge V1.1.0

## Added

- **Per-inverter Winter/Emergency Mode:** SmartPLC controls Register `0x0007`
  bit 14 and Register `0x011F` using a safety-ordered, readback-verified
  sequence.
- **Hold A for five seconds:** request Normal Mode.
- **Hold C for five seconds:** request Winter Mode.
- **Hold A+C for five seconds:** unchanged local rollback to the previous
  verified firmware. B cancels any hold gesture.
- LCD and Windows animations show a snowman only on an inverter whose Winter
  state was actually verified.
- A visible read/write/readback progress overlay now distinguishes
  `INVERTER 1/2` from `STEP 1/6`.

## Control sequence

| Target | Safe sequence for each inverter |
|---|---|
| **Winter** | Read Registers 7 and 287 → write and verify `0x011F = 1000` (100.0 A) → enable Register 7 bit 14 and verify |
| **Normal** | Read Registers 7 and 287 → disable Register 7 bit 14 and verify → write and verify `0x011F = 30` (3.0 A) |

Values already at the requested state are read but **not rewritten**. In a
multi-inverter installation, one failed device no longer removes the verified
state or snowman from successful devices. Hold the same mode key again to
retry devices that are not yet verified.

## Reliability and safety

- Register 7 changes only when its read value differs from the requested state.
- Every change uses bounded retries followed by readback verification.
- A failed Winter transition restores only that reachable inverter to the
  safe Normal state; already verified devices are not unnecessarily rewritten.
- Inverter, PV, RS485, Wi-Fi, RSE-profile and OTA settings remain preserved
  through the update.
- Long-hold gestures stay locked until A, B and C have all been observed
  released once after startup.

## Important USB note

> [!CAUTION]
> Fully close SolarPower and other automatic COM-port scanners before using the
> 14a Bridge Configurator. A third-party program that opens the SmartPLC COM
> port and toggles DTR/RTS can reset the controller. SolarPower and SmartPLC
> must not use the same USB COM port.

## Verification before release

- PlatformIO production build passed: RAM 27.2%, Flash 41.0%.
- Windows GUI self-test passed.
- V1.1.0 was USB-flashed to a physical SmartPLC and read back successfully.
- Saved inverter configuration remained present after flashing.
- Normal Mode verified on a 10 kW inverter: AC Charge disabled,
  `0x011F = 30`.
- Winter Mode verified on a 10 kW inverter: AC Charge enabled,
  `0x011F = 1000`.

The Windows installer is not Authenticode-signed. Download it only from the
official GitHub Release and verify `SHA256SUMS_V1.1.0.txt`.
