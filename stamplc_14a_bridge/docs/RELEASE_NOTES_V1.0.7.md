# V1.0.7

[Deutsch](RELEASE_NOTES_V1.0.7.de.md) · **English** ·
[繁體中文](RELEASE_NOTES_V1.0.7.zh-TW.md)

## OTA rollback hardening

- Requires ESP32 bootloader application rollback support at compile time. A
  future framework without rollback enabled now fails the production build
  instead of silently producing an unsafe OTA image.
- Keeps the current firmware active when download, signature, size, SHA-256,
  safety-gate, partition-write, or activation verification fails.
- Uses the ESP-IDF bootloader rollback state machine while a new application is
  `PENDING_VERIFY`, with the recorded previous OTA partition as a defensive
  fallback.
- Holds all inverter RS485 activity during the 15-second first-boot validation
  window.
- Automatically returns to the previous valid application if the candidate
  restarts before validation, fails its configuration self-test, or cannot be
  marked valid.
- Preserves the existing CRC-protected inverter, PV, RS485 and RSE-profile
  configuration plus Wi-Fi and OTA preferences in NVS.

## Local recovery

- Records the exact previous verified firmware partition and version after a
  successful OTA validation.
- Adds a protected A+C five-second local rollback gesture. B cancels and locks
  the gesture until every button is released.
- Binds the recorded previous partition to its immutable ELF SHA-256 before
  selecting it; missing, stale or overwritten records are rejected even when
  the Arduino framework metadata does not contain the product version.
- Requires the same safe state as OTA installation before local rollback:
  stable physical 100%, LIVE mode, idle Modbus and every enabled inverter
  verified at its target. Unsafe rollback requests are rejected without reboot.
- Disables automatic OTA after a successful local rollback so the rejected
  version is not reinstalled unattended.
- Adds a circular LCD countdown with segmented progress, a moving scanner and
  a large central seconds display.
- Reports no newer version as `AVAILABLE=-` and preserves the signed-manifest
  verification detail for GUI diagnostics.
- Opens automated-test serial sessions without toggling DTR/RTS, preventing an
  unintended controller reset from invalidating the DI stability window.

## Compatibility

- Existing commissioned RSE profiles and all saved settings remain unchanged.
- Strict 4-contact remains the default for new or reset devices.
- V1.0.7 must pass the physical release checklist before the signed manifest is
  published for automatic customer OTA.
