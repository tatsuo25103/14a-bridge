# 14A Bridge release process

This checklist is required for every production firmware release.

## Security rule

- Keep the OTA private key outside the Git repository.
- Back up the private key in an encrypted, access-controlled offline location.
- Never email, upload, commit, or include the private key in an installer.
- Devices accept only manifests signed by the matching public key embedded in firmware.

The current workstation key is stored outside the project at:

`C:\Users\lf.wu\Documents\14a Logger\.release-secrets\14a_bridge_ota_private.pem`

## Build and sign

1. Set the same version in firmware, GUI, installer, and documentation.
2. Build the firmware with PlatformIO.
3. Copy the final `firmware.bin` to both release firmware locations.
4. Sign `release/ota_manifest.json` with `tools/sign_ota_manifest.py`.
5. Verify the packaged manifest and firmware with `tools/verify_ota_release.py`.
6. Run the GUI self-test and UI self-test.
7. Build the Windows installer and test a clean installation.
8. Run `git diff --check` and confirm the private key is not tracked.

## Publish order

1. Merge the reviewed release files to `main`.
2. Create the matching GitHub release and upload:
   - `14a_bridge_firmware.bin`
   - `ota_manifest.json`
   - the Windows installer
3. Confirm both the primary raw-GitHub URL and backup GitHub Release URL return the exact signed firmware size and SHA-256.
4. USB-flash one test SmartPLC and confirm:
   - saved inverter and Wi-Fi settings remain unchanged;
   - the GUI shows the installed version and OTA diagnostics;
   - `ota check` reports a valid signed manifest;
   - no RS485 write occurs during OTA first-boot validation.
5. Release to customer devices only after the test unit passes.

If signing or verification fails, do not publish. If the private key is lost, existing shipped devices cannot trust a newly generated signing key; key rotation must first be delivered through firmware signed with the old key.
