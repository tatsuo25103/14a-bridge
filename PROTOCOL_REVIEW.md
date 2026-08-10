# P17 Modbus protocol review

Reviewed against `Modbus protocol for P17.pdf` and the connected inverter on
COM12 on 2026-07-23.

## Confirmed control definition

| Item | Confirmed value |
|---|---|
| Raw register address | `0x04E5` (decimal 1253) |
| Meaning | Maximum power limit to feed to grid |
| Unit | W |
| Width | 2 consecutive 16-bit registers |
| Read | FC03 |
| Write | FC16 |
| FC16 quantity | 2 |
| FC16 byte count | 4 |
| Register/data byte order | Most-significant byte first |
| 32-bit word order | High word first |
| RTU CRC byte order | CRC low byte, then CRC high byte |

The PDF's application examples explicitly use FC03 for reads and FC16 for
writes. Its 32-bit example decodes bytes `00 00 75 30` as decimal 30000, which
confirms the high-word-first interpretation. Actual ID 3 traffic also confirms
the request and response formats.

The PDF does not explicitly label `0x04E5` as signed or unsigned. Treating this
non-negative power limit as an unsigned 32-bit value is consistent with the
table, the examples, and the observed device behavior.

## Important PDF address error

Page 11 prints the following two setting-range entries:

- `0x016D`, decimal 365, size 2: upper limit of maximum feeding power
- `0x016E`, decimal 367, size 2: lower limit of maximum feeding power

The second row is internally inconsistent: decimal 367 is hexadecimal
`0x016F`, not `0x016E`. The first size-2 value also already occupies registers
`0x016D` and `0x016E`.

Read-only hardware probing confirms the intended layout:

| Start address | Raw 32-bit result | Interpretation |
|---|---:|---|
| `0x016D` | 15000 | Upper limit, 15000 W |
| `0x016E` | 983040000 (`0x3A980000`) | Misaligned cross-boundary read |
| `0x016F` | 0 | Lower limit, 0 W |

Therefore the lower-limit start address should be treated as `0x016F`.

## Additional relevant registers

| Address | Size | Meaning |
|---|---:|---|
| `0x03F9` | 2 | Output rated VA |
| `0x016D` | 2 | Upper limit of maximum feeding power |
| `0x016F` | 2 | Lower limit of maximum feeding power (corrected address) |
| `0x0002`, bit 9 | bit | Feed-power over-voltage derating enabled |
| `0x0002`, bit 8 | bit | Feed-power over-frequency derating enabled |
| `0x0007`, bit 13 | bit | Feed power to utility enabled |
| `0x038A` | 1 | Setting-change flag |

ID 3 returned 15000 from both output-rated-VA register `0x03F9` and the
maximum-feed upper-limit register `0x016D`; its lower limit at `0x016F` was 0.
This means the failed 10000 W readback was not caused by the documented
maximum/minimum setting range. Another controller restoring 5000 W, or separate
device logic, remains more likely.

The reported 15000 VA rating should be compared with the physical nameplate and
parallel-system configuration; it does not match the assumed "10 kW" label.

Registers `0x04E2`, `0x04E3`, and `0x04E4` are per-phase feed-grid calibration
power values. Register `0x05B3` is another feed-grid calibration value. They are
not substitutes for the `0x04E5` maximum feed limit.

## Related-setting read-only check

The following ID 3 values were read after the feed-limit tests:

| Address | Value | Interpretation |
|---|---:|---|
| `0x0002` | `0x0080` | Over-voltage bit 9 and over-frequency bit 8 are both clear |
| `0x0007` | `0xE080` | Feed-to-utility bit 13 and parallel-output bit 7 are set |
| `0x038A` | `0x0001` | A setting-change notification is present |
| `0x0124` | `0x0000` | Automatic PF adjustment is disabled |
| `0x035D` | `0x0064` | Feed-in power factor is 1.00 |
| `0x04B0` | `0x0032` | Automatic-PF start percentage is 50% |

None of these readings shows a prerequisite that must be enabled before writing
`0x04E5`. Feed-to-utility and parallel output are already enabled, while the two
documented feed-power derating bits are clear.

Register `0x038A` should not be treated as an Apply or Save command. Related
P17 status documentation describes a value of 1 as a notification that settings
changed and that the controller should query all settings again. Writing it is
not documented as part of an `0x04E5` transaction.

## What the P17 PDF does not specify

The supplied PDF contains no baud rate, parity, stop-bit, RS485, or RTU timing
definition. The working 19200 baud, 8N1 settings are confirmed empirically but
cannot be attributed to this PDF.

It also does not define arbitration for multiple controllers on one RS485 bus.
Standard Modbus serial line operation assumes one client/master issuing
requests. Frame filtering can reject an unrelated FC16 response, but it cannot:

- prevent two controllers from transmitting simultaneously;
- prove which request produced an FC03 response, because FC03 responses do not
  echo the requested register address;
- prevent another controller from immediately overwriting a successful value.

For definitive testing, the existing logger/EMS should be disconnected or the
application should communicate through the system's designated controller.

## SolarPower export for the reported second/master inverter

`SolarPower.pdf` reports the following for serial number `96162210100863`:

- Maximum active power inverter: 15000 W
- Grid standard: VDE0126
- P(f) over/under-frequency active-power control: disabled
- P(U) voltage active-power control: disabled
- Fixed Q, fixed cosphi, Q(U), and cosPhi(P): disabled

This confirms that 15000 W is a configured device rating, not merely a generic
P17 table limit. It does not, however, identify the device's Modbus unit ID or
document its parallel master/slave role.

The supplied SolarPower screenshots also need reconciliation before changing
settings: the Parameters window appears to select "Excess Electricity to the
Grid: Disable", while the MyPower Management window checks "Allow to feed-in to
the Grid". These may be stale views, different-device views, or unapplied
settings. They should not be used to infer a required Modbus write until the
physical inverter, serial number, and Modbus ID are mapped unambiguously.

## SolarPower export for the reported third/slave inverter

The later `SolarPower.pdf` export replaced the earlier file and identifies:

- Serial number: `96162207600040`
- Role reported by the operator: third inverter, parallel slave
- Maximum active power inverter: 10000 W
- Grid standard: VDE0126
- P(f) and P(U) active-power controls: disabled
- Fixed Q, fixed cosphi, Q(U), and cosPhi(P): disabled

This 10000 W active-power rating exactly matches the observed ID 3 behavior:
ID 3 accepted 15000 W at the FC16 protocol level but retained 10000 W on
readback. It is therefore likely that ID 3 is the third/slave inverter, while
ID 2 is the second/master inverter. This supersedes the earlier working
assumption that ID 3 was the parallel master.

The earlier ID 3 read of 15000 at `0x03F9` is not contradictory: P17 defines
`0x03F9` as rated VA (apparent power), while the SolarPower export specifies a
10000 W maximum active power. Similarly, the 15000 W value at `0x016D` is a
configuration-range upper bound and did not override the device's 10000 W
active-power limit.

The third/slave screenshots repeat the same feed-permission display conflict as
the second/master screenshots: MyPower Management checks "Allow to feed-in to
the Grid", while Parameters appears to select "Excess Electricity to the Grid:
Disable". Because ID 3's Modbus `0x0007` bit 13 was read as enabled, the MyPower
Management state agrees with the live Modbus value. The Parameters radio view
may be stale or represent a different application state and should not be
blindly reapplied.

## ID 2 translated FC16 response

The observed ID 2 frame `02 10 09 CA 00 04 E2 5B` is mathematically related to
the request (`0x04E5 * 2 = 0x09CA`, two registers become four bytes), but it is
not a standard Modbus FC16 acknowledgement. It must not be reported as proof of
a successful write.

The application now treats it only as a provisional, device-specific response
and verifies the setting with delayed FC03 reads. If none of those reads equals
the requested value, the operation is reported as failed. This improves the
diagnosis but cannot make firmware retain a value that the parallel controller
rejects or overwrites.
