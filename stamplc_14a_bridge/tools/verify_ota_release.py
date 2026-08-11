#!/usr/bin/env python3
"""Verify a packaged 14A Bridge OTA release exactly as the device expects."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from sign_ota_manifest import (
    HARDWARE,
    SIGNATURE_ALGORITHM,
    canonical_payload,
    openssl_path,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    firmware = args.firmware.read_bytes()
    expected_size = len(firmware)
    expected_sha = hashlib.sha256(firmware).hexdigest()
    checks = {
        "hardware": manifest.get("hardware") == HARDWARE,
        "signature_alg": manifest.get("signature_alg") == SIGNATURE_ALGORITHM,
        "size": manifest.get("size") == expected_size,
        "sha256": manifest.get("sha256") == expected_sha,
        "primary_url": str(manifest.get("primary_url", "")).startswith("https://"),
        "backup_url": str(manifest.get("backup_url", "")).startswith("https://"),
    }
    failed = [name for name, valid in checks.items() if not valid]
    if failed:
        raise SystemExit("OTA release verification failed: " + ", ".join(failed))

    payload = canonical_payload(
        manifest["version"], manifest["size"], manifest["sha256"],
        manifest["primary_url"], manifest["backup_url"],
    )
    signature = base64.b64decode(manifest["signature"], validate=True)
    with tempfile.TemporaryDirectory(prefix="14a-ota-verify-") as temporary:
        payload_path = Path(temporary) / "manifest.txt"
        signature_path = Path(temporary) / "manifest.sig"
        payload_path.write_bytes(payload)
        signature_path.write_bytes(signature)
        subprocess.run(
            [openssl_path(), "dgst", "-sha256", "-verify", str(args.public_key),
             "-signature", str(signature_path), str(payload_path)],
            check=True,
        )
    print(f"OTA release verified: version={manifest['version']} "
          f"size={expected_size} sha256={expected_sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
