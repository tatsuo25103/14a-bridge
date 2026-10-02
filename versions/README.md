# Version snapshots

Each released development version is stored in its own folder:

```text
versions/
└── vX.Y.Z/
    ├── source/       Buildable source code
    ├── firmware/     Firmware binaries, bootloader and partitions
    ├── installer/    Windows installer
    ├── docs/         Release documentation and images
    └── checksums/    SHA-256 checksums and version metadata
```

Generated environments and caches such as `.venv`, `.pio`, temporary build
directories and Python caches are not copied into snapshots. Git tags remain
the authoritative source history; these folders are local, independent release
packages for convenient maintenance and recovery.
