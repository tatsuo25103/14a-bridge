# 14a Bridge production reliability test

This checklist is required before a firmware build is approved for customer
shipment. Passing software compilation alone is not a ten-year lifetime claim.

## Automated gates

- Firmware builds with all `ControlPolicyCompileTests.cpp` assertions enabled.
- Windows GUI `--self-test` and `--ui-self-test` return exit code 0.
- USB parser tests pass.
- OTA manifest signature and firmware SHA-256 are verified after public download.
- A 168-hour read-only soak run finishes with zero unexpected resets and zero
  communication failures:

  ```powershell
  python tools/soak_test.py --port COM5 --hours 168 --interval 60
  ```

Close the GUI before starting the soak monitor. It sends only `identify` and
`gui`; it does not access the RS485 inverter bus.

## Power interruption matrix

Repeat each case at least 20 times and confirm the previous complete settings
remain available after reboot:

1. Remove power while saving inverter settings.
2. Remove power while the GUI writes firmware; then recover by USB flash.
3. Remove power during OTA download, before activation.
4. Remove power immediately after OTA reboot and verify rollback.
5. Remove and restore the 12 V supply with USB disconnected and connected.

## OTA rollback qualification

Run these tests on the exact bootloader, partition table, and firmware image
that will be shipped. Confirm that inverter RS485 remains untouched until the
new application has passed its 15-second validation window.

1. Interrupt Wi-Fi during download: the running version must remain active.
2. Corrupt the downloaded image/hash: activation must be rejected.
3. Boot an intentionally crashing candidate: the ESP32 bootloader must return
   to the previously valid application without USB recovery.
4. Force the candidate configuration self-test to fail: it must be marked
   invalid and the previous application must boot automatically.
5. Force `esp_ota_mark_app_valid_cancel_rollback()` to fail: the candidate must
   not clear its rollback record or enter inverter control.
6. Power-cycle repeatedly inside the 15-second validation window: after one
   unsuccessful candidate boot, the previous application must be restored.
7. After every rollback, confirm inverter, PV, RS485, Wi-Fi, RSE-profile and
   OTA settings are unchanged and the rollback reason is visible over USB.

## RSE and RS485 fault injection

- Exercise all 16 DI masks; only `0x01`, `0x02`, `0x04`, and `0x08` are valid.
- Switch RSE during every stage of a six-inverter write batch. The old batch
  must stop and the newest stable physical RSE must be applied to every ID.
- Disconnect A, B, and GND separately; reverse A/B; short A/B through an
  appropriate protected test fixture; inject CRC errors and delayed replies.
- Confirm one or two failures remain `CHECK`; only three consecutive failures
  become `ERROR`, and a recovered inverter automatically returns to `OK`.
- Confirm an offline or changed rating is `PENDING` and excluded from control.
- Confirm rating validation cannot start in SAFE, TEST, invalid RSE, 0%, 30%,
  or 60%; it is permitted only in LIVE with physical RSE at 100%.

## Environmental and compliance validation

- Temperature cycling at the intended enclosure limits.
- Supply brownout, surge, ESD, EFT, and conducted/radiated EMC testing using
  the final 12 V supply, enclosure, cable lengths, and grounding arrangement.
- RS485 multi-drop testing with six inverters and the maximum supported cable.
- Verify the RSE response time against the applicable grid operator and §14a
  requirements.

Record hardware revision, firmware hash, inverter models, test equipment,
dates, results, and operator for every production qualification run.
