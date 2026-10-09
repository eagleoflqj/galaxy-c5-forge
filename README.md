# Galaxy C5 Forge

Reproducible, evidence-driven bring-up work for an unofficial LineageOS build for the Samsung Galaxy C5 SM-C5000.

The confirmed source-tree device name is `c5lte`; Samsung stock fingerprints also use `c5ltechn`. Those names are not treated as interchangeable until stock firmware or a real device confirms each context.

The LineageOS 16.0 (Android 9) bring-up milestone is closed for the designated CSA1 device. The final recovery passes display, touch, native root ADB, sideload transitions, BCB access and required partition discovery. The installed ROM passes the tested boot, Wi-Fi, Bluetooth, audio including wired-headset output, camera, display/input, storage, USB, NFC, fingerprint, GNSS, compass and sensor scopes under SELinux enforcing. The exact CSA1 sensor-hub firmware/HAL pair repaired step counting: Paseo worked, SensorService recorded a 0-to-48 event sequence and the former invalid-FIFO errors did not recur. The proximity sensor also passes a phyphox Proximity timer cover/uncover test. SIM/RIL/telephony was explicitly excluded, while gesture sensing, earpiece/wired-headset microphone paths and long-duration stability remain documented as untested rather than implied PASS. One PeripheralManager startup mismatch remains a non-blocking GNSS warning. Proprietary files remain outside Git. P5 reproducibility/CI remains unfinished. The LineageOS 17.1 (Android 10) adaptation milestone is also closed for the same owner's no-SIM use as of 2026-10-08. Final installed ROM SHA-256 `daff6dce9a65b162e4b77753ab04aa200220cfe62960ed6e2cda1154770cbaa4` passes the selected hardware scopes, including fresh GPS, sustained power-off/cold boot with automatic Wi-Fi reconnect, Bluetooth headset playback/reconnect and MTP file round trip. Read-only storage/runtime checks corroborate normal boot, writable data/cache, enforcing SELinux and an empty captured crash buffer; RIL registration stability is tested without claiming cellular support or extended endurance. Earlier camera, fingerprint, brightness, power-off, recovery-loop and USB-replug defects are repaired for their recorded scopes. Audit warnings and untested functions remain documented; independent clean rebuilding/CI and byte-identical reproduction are not complete. See `docs/test-matrix.md`, `docs/lineage-17.1.md` and `docs/status.md` for exact scope, provenance and limitations. No further flashing or device-data changes are authorized by default.

Start with:

```bash
./scripts/doctor.sh
./scripts/audit-sources.sh
./scripts/setup.sh --workdir /absolute/path/to/android-work
./scripts/build.sh --workdir /absolute/path/to/android-work --target recovery
./scripts/build.sh --workdir /absolute/path/to/android-work --target rom
./scripts/import-stock-gnss.sh --workdir /absolute/path/to/android-work --system-image /absolute/path/to/system.raw.ext4
./scripts/import-stock-sensors.sh --workdir /absolute/path/to/android-work --system-image /absolute/path/to/system.raw.ext4
./scripts/import-stock-pm-proxy.sh --workdir /absolute/path/to/android-work --system-image /absolute/path/to/system.raw.ext4
./scripts/build.sh --workdir /absolute/path/to/android-work --target rom --installclean
```

For the Android 10 worktree, add `--lineage-version 17.1` to `scripts/setup.sh` and `scripts/build.sh`, and set `LINEAGE_VERSION=17.1` when invoking `scripts/lineage-env.sh` directly.

Read `docs/status.md`, `docs/inventory.md`, `docs/hardware.md`, `docs/baseline-decision.md`, `docs/recovery-and-flashing.md`, `docs/test-matrix.md`, and `docs/lineage-17.1.md` before building or testing anything. Exact Android 9 build results and artifact hashes are in `docs/build.md`; Android 10 migration evidence is in `docs/lineage-17.1.md`.

## License

This repository's original orchestration code and documentation are licensed under the Apache License 2.0. External Android sources, Linux kernel code, and proprietary files retain their own licenses and are not relicensed by this repository. See [LICENSE](LICENSE).
