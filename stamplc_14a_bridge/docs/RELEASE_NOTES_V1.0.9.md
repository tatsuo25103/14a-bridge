# 14a Bridge V1.0.9

## Added

- Two separate RS485 segments prevent the previous multi-master
  collision between PlexLog and SmartPLC:
  - PlexLog inverter port → external automatic-direction RS485/UART adapter →
    StampPLC **PORT.C** (`GPIO4/RX`, `GPIO5/TX`)
  - StampPLC **PWR485** → P17 cards / inverter IDs `2-7`
- **Enable PlexLog gateway** on the Settings page. It defaults to enabled after
  schema-5 migration and can be persistently disabled.
- Per-inverter PlexLog diagnostics plus forwarded, cache, queue, expiration,
  rejection and maximum-latency counters.

## Communication behavior

- PlexLog has priority during steady operation. SmartPLC background health
  reads start only in a measured PlexLog idle gap.
- SmartPLC has priority during an RSE transition, writes and verifies the new
  limit, and provides one bounded PlexLog service slot between controlled
  inverters.
- Successful FC03/FC04 responses are cached only for an exact ID, function,
  address and quantity match. Writes and Modbus exceptions are never
  fabricated or served from cache.
- Only structurally valid requests for inverter IDs `2-7` reach the P17 bus.
  Meter ID `1`, broadcasts and unsupported frames are rejected.
- Power verification starts after 140 ms. If a valid readback still contains
  the previous value, SmartPLC performs a second read-only check before another
  write can occur. The three-write-attempt maximum is unchanged.

## Required wiring change

PlexLog must no longer remain in parallel with SmartPLC on the P17 bus. Connect
the PlexLog inverter port only through an automatic-direction RS485 adapter to
PORT.C. Keep PlexLog's separate meter port directly connected to the meter
(ID 1). There is no bypass relay; if SmartPLC is powered off or the gateway is
disabled, PlexLog intentionally reports lost inverter communication.

## Verification performed before release

- PlatformIO production build passed: RAM 27.1%, Flash 40.6%.
- 14/14 automated firmware, GUI and capacity tests passed.
- One-inverter soak sample: 191 PlexLog requests over 175 seconds with no new
  downstream failures, drops or expirations.
- Two complete deliberate collision rounds
  `100 → 60 → 30 → 0 → 100`: 10/10 inverter write/readback checks and 100/100
  PlexLog responses, with zero timeout, drop or downstream failure.

## Windows download notice

The V1.0.9 Windows installer is not Authenticode-signed. Windows SmartScreen
may therefore show an unknown-publisher warning. Download only from the
official GitHub Release and verify `SHA256SUMS_V1.0.9.txt` before installation.

> A physical six-inverter acceptance test remains required before a six-unit
> installation is commissioned. The software capacity model does not replace
> that site test.
