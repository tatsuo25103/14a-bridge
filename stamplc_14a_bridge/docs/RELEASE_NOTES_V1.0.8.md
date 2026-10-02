# 14a Bridge V1.0.8

## Added

- Four independently editable RSE power targets for 100%, 60%, 30% and 0%.
  Initial values are calculated from installed PV power and capped by the
  verified inverter rating; an installer may then enter approved rounded or
  site-specific values.
- A separate **Feedin Enable** option for every RSE level.
- Persistent schema-4 storage for the new values. Existing schema-1/2/3
  installations migrate automatically and retain their previous settings.

## Changed and corrected

- Every required write to P17 Register `0x04E5` is followed by FC03 readback.
  Verification is limited to three attempts; retries are never unlimited.
- If all three power-verification attempts fail, the controller sends the P17
  feed-in-disable command (`0xDFFF`) to Register `0x0007`, Bit 13, and verifies
  the resulting bit state.
- During normal control, Register `0x0007` is read before use and is written
  only when its confirmed state differs from the requested Feedin Enable
  state. An unreadable state is left unchanged except for the bounded
  power-write fail-safe above.
- Disabling Feedin Enable shows an effective output of 0 W on the SmartPLC and
  GUI while `0x04E5` is still programmed to the configured power. The raw
  register value remains available in diagnostics.
- Periodic health monitoring verifies both power and feed-permission state
  without repeatedly rewriting unchanged values.

> **Operational warning:** changing Register `0x0007`, Bit 13 can stop or
> restart inverter grid feed-in. Commission each RSE level under supervision
> and use only values approved for the site.

## Verification performed before release

- PlatformIO production firmware build passed.
- OTA manifest signature, firmware size and SHA-256 verification passed.
- Windows GUI built-in logic and UI self-tests passed.
- SmartPLC V1.0.8 physical DI test passed for IN1–IN4 and return-to-open state.
- RS485 FC03 test to inverter ID2 passed 20/20 reads at 19200 baud with no
  timeout or CRC error.
