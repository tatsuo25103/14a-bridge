#!/usr/bin/env python3
"""Create a signed 14A Bridge OTA manifest without exposing the private key."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


HARDWARE = "M5STACK_STAMPPLC"
SIGNATURE_ALGORITHM = "ECDSA_P256_SHA256"


def openssl_path() -> str:
    found = shutil.which("openssl")
    if found:
        return found
    windows_git = Path(r"C:\Program Files\Git\usr\bin\openssl.exe")
    if windows_git.exists():
        return str(windows_git)
    raise SystemExit("OpenSSL was not found")


def canonical_payload(version: str, size: int, sha256: str,
                      primary_url: str, backup_url: str) -> bytes:
    return (
        "14A_BRIDGE\n"
        f"{HARDWARE}\n"
        f"{version}\n"
        f"{size}\n"
        f"{sha256}\n"
        f"{primary_url}\n"
        f"{backup_url}\n"
    ).encode("utf-8")


def public_der(openssl: str, key: Path, private: bool) -> bytes:
    command = [openssl, "pkey"]
    if not private:
        command.append("-pubin")
    command += ["-in", str(key), "-pubout", "-outform", "DER"]
    return subprocess.check_output(command)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--primary-url", required=True)
    parser.add_argument("--backup-url", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    firmware = args.firmware.read_bytes()
    sha256 = hashlib.sha256(firmware).hexdigest()
    size = len(firmware)
    payload = canonical_payload(args.version, size, sha256,
                                args.primary_url, args.backup_url)
    openssl = openssl_path()

    if public_der(openssl, args.private_key, True) != public_der(
            openssl, args.public_key, False):
        raise SystemExit("Private key does not match the embedded OTA public key")

    with tempfile.TemporaryDirectory(prefix="14a-ota-sign-") as temporary:
        payload_path = Path(temporary) / "manifest.txt"
        signature_path = Path(temporary) / "manifest.sig"
        payload_path.write_bytes(payload)
        subprocess.run(
            [openssl, "dgst", "-sha256", "-sign", str(args.private_key),
             "-out", str(signature_path), str(payload_path)],
            check=True,
        )
        subprocess.run(
            [openssl, "dgst", "-sha256", "-verify", str(args.public_key),
             "-signature", str(signature_path), str(payload_path)],
            check=True,
        )
        signature = base64.b64encode(signature_path.read_bytes()).decode("ascii")

    manifest = {
        "version": args.version,
        "hardware": HARDWARE,
        "size": size,
        "url": args.primary_url,
        "primary_url": args.primary_url,
        "backup_url": args.backup_url,
        "sha256": sha256,
        "signature_alg": SIGNATURE_ALGORITHM,
        "signature": signature,
    }
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"signed {args.output}: version={args.version} size={size} sha256={sha256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
