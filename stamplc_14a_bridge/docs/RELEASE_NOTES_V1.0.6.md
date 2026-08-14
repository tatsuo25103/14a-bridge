# V1.0.6

## Added

- Selectable RSE truth-table profiles in the Windows GUI and SmartPLC:
  - Strict 4-contact (legacy)
  - Westnetz 4-contact
  - EWE 4-contact with hold-last behaviour
  - VDE FNN / Netze BW 3-contact EZA
- Complete compile-time regression tables for all 16 DI1-DI4 combinations in every profile.
- Saved RSE profile in the CRC-protected, dual-slot SmartPLC configuration.

## Safety changes

- A LIVE GUI test can only keep or further reduce the physical RSE command. It cannot bypass a physical 0%, 30%, or 60% reduction.
- A physical RSE transition immediately terminates TEST and reapplies the physical command.
- SmartPLC OTA installation requires stable physical 100%, no TEST, idle Modbus, and all enabled inverters verified and ready.
- The OTA safety condition is rechecked during download; a changed RSE state aborts installation.

## Fixed

- Settings table width is recalculated after the final Windows DPI/layout pass, keeping Readback and Status visible.
- The RS485, RSE profile, action buttons, and status notice use two fixed rows so controls no longer disappear or overlap.
- `HOLD` is reported separately from an invalid RSE mask for EWE profile diagnostics.

## Upgrade compatibility

- Schema-1 and schema-2 settings migrate to schema 3 without losing inverter, installed PV, verified inverter limit, RS485, Wi-Fi, or OTA settings.
- Existing devices retain the legacy strict 4-contact profile until an installer deliberately selects another profile.
- Previous Git tags, GitHub Releases, installers, and release assets remain unchanged and available.
