# Galaxy C5 Forge

Reproducible, evidence-driven bring-up work for an unofficial LineageOS build for the Samsung Galaxy C5 SM-C5000.

The confirmed source-tree device name is `c5lte`; Samsung stock fingerprints also use `c5ltechn`. Those names are not treated as interchangeable until stock firmware or a real device confirms each context.

The LineageOS 16.0 (Android 9) bring-up milestone is closed for the designated CSA1 device. The final recovery passes display, touch, native root ADB, sideload transitions, BCB access and required partition discovery. The installed ROM passes the tested boot, Wi-Fi, Bluetooth, audio including wired-headset output, camera, display/input, storage, USB, NFC, fingerprint, GNSS, compass and sensor scopes under SELinux enforcing. The exact CSA1 sensor-hub firmware/HAL pair repaired step counting: Paseo worked, SensorService recorded a 0-to-48 event sequence and the former invalid-FIFO errors did not recur. The proximity sensor also passes a phyphox Proximity timer cover/uncover test. SIM/RIL/telephony was explicitly excluded, while gesture sensing, earpiece/wired-headset microphone paths and long-duration stability remain documented as untested rather than implied PASS. One PeripheralManager startup mismatch remains a non-blocking GNSS warning. Proprietary files remain outside Git. P5 reproducibility/CI and any modern-Android work are separate, unfinished follow-up phases; no further flashing or device-data changes are authorized by default.

Start with:

```bash
./scripts/doctor.sh
./scripts/audit-sources.sh
./scripts/setup.sh --workdir /absolute/path/to/android-work
./scripts/build.sh --workdir /absolute/path/to/android-work --target recovery
./scripts/build.sh --workdir /absolute/path/to/android-work --target rom
./scripts/import-stock-gnss.sh --workdir /absolute/path/to/android-work --system-image /absolute/path/to/system.raw.ext4
./scripts/import-stock-sensors.sh --workdir /absolute/path/to/android-work --system-image /absolute/path/to/system.raw.ext4
./scripts/build.sh --workdir /absolute/path/to/android-work --target rom --installclean
```

Read `docs/status.md`, `docs/inventory.md`, `docs/hardware.md`, `docs/baseline-decision.md`, `docs/recovery-and-flashing.md`, and `docs/test-matrix.md` before building or testing anything. Exact build results and artifact hashes are in `docs/build.md`.

## License

This repository's original orchestration code and documentation are licensed under the Apache License 2.0. External Android sources, Linux kernel code, and proprietary files retain their own licenses and are not relicensed by this repository. See [LICENSE](LICENSE).
