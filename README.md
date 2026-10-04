# Galaxy C5 Forge

Reproducible, evidence-driven bring-up work for an unofficial LineageOS build for the Samsung Galaxy C5 SM-C5000.

The confirmed source-tree device name is `c5lte`; Samsung stock fingerprints also use `c5ltechn`. Those names are not treated as interchangeable until stock firmware or a real device confirms each context.

The current development baseline is LineageOS 16.0 (Android 9). Recovery and a full ROM ZIP compile successfully, but neither has been tested on hardware. The first hardware milestone is still a recovery image that boots and exposes ADB. No flashing, unlocking, partition writes, or device-data changes are authorized by default.

Start with:

```bash
./scripts/doctor.sh
./scripts/audit-sources.sh
./scripts/setup.sh --workdir /absolute/path/to/android-work
./scripts/build.sh --workdir /absolute/path/to/android-work --target recovery
./scripts/build.sh --workdir /absolute/path/to/android-work --target rom
```

Read `docs/status.md`, `docs/inventory.md`, `docs/hardware.md`, and `docs/baseline-decision.md` before building or testing anything. Exact build results and artifact hashes are in `docs/build.md`.

## License

This repository's original orchestration code and documentation are licensed under the Apache License 2.0. External Android sources, Linux kernel code, and proprietary files retain their own licenses and are not relicensed by this repository. See [LICENSE](LICENSE).
