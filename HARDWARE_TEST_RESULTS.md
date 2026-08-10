# Hardware acceptance test - 2026-07-23

## Setup

- Port: COM12
- Serial: 19200 baud, 8N1
- Register: `0x04E5`
- Read: FC03, quantity 2
- Write: FC16, quantity 2
- Encoding: unsigned 32-bit, high word first
- Transaction timeout: 2.0 seconds

## ID 3

Initial FC03 read succeeded and returned 5000 W.

| Requested | FC16 response | FC03 readback | Result |
|---:|---|---:|---|
| 0 W | Matched `0x04E5`, quantity 2 | 0 W | Pass |
| 5000 W | Matched `0x04E5`, quantity 2 | 5000 W | Pass |
| 10000 W | Matched `0x04E5`, quantity 2 | 5000 W | Mismatch |
| 15000 W | Not sent | - | Stopped for safety |

The sequence stopped immediately at the 10000 W mismatch. A later read-only
check confirmed that ID 3 remained at 5000 W.

Possible explanations for the accepted FC16 response followed by a 5000 W
readback include an inverter/configuration limit or another controller restoring
the value.

Subsequent read-only protocol checks returned:

- `0x03F9` output rated VA: 15000
- `0x016D` maximum-feed upper limit: 15000 W
- corrected `0x016F` maximum-feed lower limit: 0 W

The documented range therefore permits 10000 W. A simple upper/lower range
clamp does not explain the 5000 W readback.

## ID 2 read-only observation

The read-only request received a CRC-valid ID 2 FC03 response containing
`00 00 A5 FF` (42495 W). This conflicts with the earlier observed 4351 W value
and is not considered reliable.

An FC03 response contains ID, function, byte count, and data, but not the source
register address. On a shared multi-master bus, another ID 2 FC03 response with
four data bytes can therefore be indistinguishable from the response to this
application's `0x04E5` request.
