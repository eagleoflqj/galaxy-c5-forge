# Recovery and flashing record

Last updated: 2026-10-04

This document records the designated-device procedure and current recovery limits. It is not blanket authorization to flash another device or artifact. Every future flash, wipe or device-data mutation still requires operation-specific authorization after the exact device, artifact, hash, target and recovery path are restated.

## Confirmed device and partition scope

- Model: Samsung Galaxy C5 `SM-C5000`
- Device: `c5lte`
- Bootloader: `C5000ZCU1CSA1`
- Recovery: PIT Entry #18, Identifier 19, `/dev/block/mmcblk0p19`, 33,554,432 bytes
- Boot: `/dev/block/mmcblk0p18`, 33,554,432 bytes
- System: `/dev/block/mmcblk0p36`, 3,196,059,648 bytes
- Cache: `/dev/block/mmcblk0p37`, 209,715,200 bytes
- Userdata: `/dev/block/mmcblk0p38`, 58,787,344,384 bytes
- FOTA-backed misc/BCB: `/dev/block/mmcblk0p21`, 5,243,904 bytes

PIT writes, repartitioning, bootloader, modem, EFS, persist and FRP/config writes are not part of the recovery or ROM procedure.

## Installed and verified recovery

The installed final Lineage recovery image is 16,234,512 bytes with SHA-256 `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`. It was written only to Heimdall target `RECOVERY` under explicit authorization after a live PIT preflight. It boots the source-built Linux 3.10.108 kernel and passes display, touch, native Android 28 root ADB, normal-to-sideload-to-normal USB transitions, BCB mapping and required partition discovery.

The complete pre-Lineage TWRP recovery partition backup is stored outside Git with mode 600 and SHA-256 `fd6a2b60d821ae157e97ca2c772ddd4140d8f1dc657193ca5db3939d4e649402`. The eighth root-ADB diagnostic recovery is also stored outside Git with SHA-256 `fb6527da736bddb42895b0590e20fc344ccabc28c834822c9163772c3865d897`. Either can be written only to `RECOVERY` through Download Mode after a new exact preflight and authorization.

## Installed ROM baseline

The installed ROM was produced from a 618,085,215-byte ZIP with SHA-256 `f6d6d9e0c6bcf9762a61662ae68613457391fa6ef16d4f974a568bd954e4b52c`. It is an unofficial userdebug build signed with the source tree's test keys, not a production release. A later build now occupies the standard output filename, so that path must not be used to identify the installed bytes.

The updater asserts `c5lte` and bootloader `C5000ZHU1CRJ5` or `C5000ZCU1CSA1`. Static inspection confirms it performs a block-image update of `system` and extracts `boot.img` to `boot`; it does not directly write recovery, PIT, modem, EFS or other firmware partitions. The included boot image is 11,114,512 bytes with SHA-256 `2e9274e34014d4c4a6c3c2a5cde55ad4f22dd0e48b357ebc28ae899e567b9bbb` and fits the measured 33,554,432-byte boot partition.

The installed system contains a recovery reconstruction script, but the corresponding init service is disabled unless `persist.sys.recovery_update=true`. The property is not preset by this build, so the currently verified recovery should remain installed.

## Wi-Fi-fix rollback baseline

The last fully booting ROM came from a 618,153,042-byte ZIP with SHA-256 `099ba78ec9ebe8320e613087901f9d1cf46d2137ee8be6be1abdf8300b6c6359`. Later builds occupy the standard output filename, so that path must not be used to identify these bytes. This update's only intentional source change from the first installed baseline added the missing system firmware link `adsp.b15 -> /firmware-modem/image/adsp.b15`; the target file already existed on the unchanged modem firmware partition.

The updater asserted `c5lte` and the CSA1 bootloader, replaced system, and wrote its 11,114,512-byte boot image with SHA-256 `369a0444bdfdc37dc6336ec2f80cf2f54bdc707a3278f3dfe009dae9c6ef5902`. It did not wipe or write userdata/cache and did not target recovery, modem, bootloader, EFS, PIT or repartitioning. Recovery and read-only post-install checks confirmed success and the device booted the update.

Immediately before the first GNSS update, the complete installed boot and system partitions were captured read-only. The 33,554,432-byte boot partition has SHA-256 `5729cb21a348b97ea852ed21421fbb43e3bdd235891dcbf512f6bc23fcbff378`; the 3,196,059,648-byte system partition has SHA-256 `3f2a159ba227e680d9a69dc82fbc6662d6b8af00897bb8986bb9893721a48773`. Their mode-600 zstd copies pass integrity testing and remain outside Git, so recovery can restore the exact last booting pair after separate authorization.

The user supplied operation-specific authorization for this exact ZIP hash. Because both software reboot-to-recovery attempts returned to Android, the user entered the verified recovery with the hardware keys, then selected `Apply update` → `Apply from ADB`. Sideload, post-install inspection and the authorized reboot all completed successfully without a factory reset.

## Installed failed GNSS candidate

The user authorized sideload of the 620,082,686-byte ZIP with SHA-256 `d3413774dc970ddf2e813dd0761abe3b54d7526785bbadc8be03f2abeae79b4d`. Recovery verified the package signature, wrote all 366,745 expected system blocks, wrote boot, recorded status `1` and `Install complete`, and preserved userdata/cache. Read-only checks matched boot prefix SHA-256 `69d0f8b161a0e4d54aa059df0b98b5452ff53d4467bf75c2fbcd8bca54318bb6` and the unchanged recovery prefix SHA-256 `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`.

The authorized reboot does not complete. Android reaches `system_server`, then waits indefinitely for `android.hardware.gnss@1.0::IGnss/default`; both `sec_gnss_service` and `loc_launcher` restart every five seconds. Linker logs show `android.hardware.gnss@1.0-impl-qti.so` cannot find `libloc_core.so` and `loc_launcher` cannot find `libgps.utils.so`. This proves that the candidate's renamed libraries and `LD_PRELOAD` mechanism did not cross the relevant Android linker namespaces. The phone remains at the boot animation with ADB available; no rollback or second update is authorized yet.

Mode-600 installation evidence remains outside Git. The actual recovery log has SHA-256 `00083ab3cbe85b9c62b63630db0b5f02be3080da0775f7f3fc9d21b147948d9a`; the host sideload transcript has SHA-256 `33e8270869afbf5bcaf8c5c53feb4d270f4f0f9d707b2e3cca693cc80dea078b`. Two small files named after `/cache/recovery/last_log` and `last_install` contain only the `No such file or directory` capture errors and are explicitly documented as absence evidence, not install status.

## Installed replacement GNSS candidate

The current host output ZIP is `/mnt/storage/android/galaxy-c5-lineage-16/out/target/product/c5lte/lineage-16.0-20261004-UNOFFICIAL-c5lte.zip`, 620,083,081 bytes, SHA-256 `d7002bdb040094d1892927a0d8d00502dd049a4ca5a3e5adee2fde13e9aadecd`. Its 11,114,512-byte boot image has SHA-256 `b97af8d517902851aa7d074c725f2f2c3389d793b5f35a41e9b085a500f2cd0a`. Reviewed mode-600 copies and the build log are preserved outside Git under `/mnt/storage/android/galaxy-c5-build-artifacts/gnss-csa1-d7002bdb`; the build log SHA-256 is `d19a84c07fa7c55d5e0d4b7e50d23263f91491004988cac43583c12038f56891`.

The replacement removes `LD_PRELOAD` and rewrites the imported ELF DT_NEEDED and SONAME strings in place to `libsecgnssfeature.so`, `libgps.csa1.so` and `libloc_c5.so`. The importer first verifies all 45 stock hashes, requires the exact rewrite counts 7/25/19, then verifies 26 post-rewrite hashes. The clean build completed in 2 minutes 38 seconds; ZIP CRC, metadata, boot header, all output hashes, rewrite counts, absence of old dependency names and generic/stale GNSS artifacts, and updater scope passed. The updater writes only system and boot and preserves userdata/cache.

The user authorized reboot to recovery, sideload of exactly this replacement hash, writes only to system/boot, preservation of userdata/cache and reboot after successful installation. Android-side preflight found one SM-C5000/c5lte device with CSA1 bootloader, 100% full battery, an exact 620,083,081-byte candidate hash match, the intended updater scope and passing zstd rollback images. The authorized `adb reboot recovery` again returned to ordinary Android, so the user entered recovery with the hardware keys. Recovery revalidation passed at 97% battery and charging. The host then sent the single authorized ZIP and returned exit code 0 with `Total xfer: 1.00x`.

The recovery log records signature verification against key 0, all 366,745 expected system blocks written, zero stashed blocks, updater status `1`, 1,502,187,520 system bytes written and `Install complete`; it contains no userdata/cache formatting record. Before reboot, the boot prefix matched `b97af8d517902851aa7d074c725f2f2c3389d793b5f35a41e9b085a500f2cd0a`, recovery remained `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`, all 45 installed GNSS hashes matched and stale artifacts were absent. Android completed the authorized reboot in about 37 seconds with stable Samsung/QTI GNSS services. Functional testing then passed: SatStat detected 17 satellites, the map identified the correct location and compass direction worked. Sanitized system state recorded 162 location reports, three TTFF reports with a mean of about 5.67 seconds, stable service processes, SELinux enforcing and no linker or fatal-process errors. No coordinates were retained. The mode-600 recovery log is stored outside Git with SHA-256 `faad50a12f08716082c7c4c060da147cf8239fa8afdb795c5c379d4812cd3a5e`; the sanitized functional record has SHA-256 `9e791720680b26277044f616e7420bdb0300f237c49a4eac2f65bbcd72902a90`.

## Installed CSA1 sensor-stack build

The current reviewed candidate is a 620,087,151-byte ZIP with SHA-256 `71e3f1a9b3b92a85e6c2bf8dcdd277aa43ff015cc5ccb7732d81208fdc8a4029`. Its 11,114,512-byte boot image has SHA-256 `90f917a9e34be163592190093b5b43bbf05ed005c3263882455351c67387cc66`. Mode-600 copies and the build log are stored outside Git under `/mnt/storage/android/galaxy-c5-build-artifacts/sensors-csa1-71e3f1a9`.

The only intentional source-input change from the second GNSS build is replacement of the public sensor-hub firmware and legacy sensor HAL with the exact hash-pinned CSA1 pair. Offline checks pass for ZIP integrity, device/bootloader assertions, boot addresses, GNSS regression hashes, sensor hashes and complete 32-bit HAL symbol closure. The updater writes only system and boot and contains no userdata/cache formatting operation or recovery, modem, bootloader, EFS, PIT or repartitioning payload.

The build was installed under explicit authorization. Sideload returned `Total xfer: 1.00x`; recovery verified the package, wrote all 366,750 expected system blocks, reported `Install complete` and contained no updater error or userdata/cache formatting record. Before reboot, the new boot prefix, unchanged recovery, 28,476-byte firmware and 120,508-byte HAL all matched their reviewed SHA-256 values. Android completed boot in about 38 seconds under SELinux enforcing. Paseo then counted steps, SensorService recorded events increasing from 0 to 48 and the kernel reported corresponding positive step differences without the former invalid-FIFO or sequential-processing errors. Wi-Fi remained `ok`, the GNSS process remained present and filtered logs contained no sensor/GNSS linker or fatal-process error.

## First installation authorization record

The clean first installation included a recovery Factory reset because the device contained Samsung Android 8 data and the Lineage build is Android 9. Factory reset destroyed the prior userdata and cache contents after the user confirmed backups and gave operation-specific authorization.

That authorization explicitly covered both actions:

1. Factory reset from the verified Lineage recovery, affecting userdata and cache.
2. `adb sideload` of the exact ROM ZIP above, allowing the signed updater to replace system and boot.

The authorization did not include any add-on package, root package, GApps package, recovery replacement, firmware update, modem/bootloader write, PIT write or repartitioning, and none was performed.

## Recorded first-install execution outline

After authorization, recheck the connected serial, model, device, bootloader, battery/charging state, exact ZIP size/hash, root recovery ADB, by-name links and block-device capacities. Run Factory reset through the recovery UI and wait for a successful completion message. Then select `Apply update` → `Apply from ADB` and run:

```bash
adb -s SERIAL sideload /absolute/path/to/lineage-16.0-20261004-UNOFFICIAL-c5lte.zip
```

Record the complete host result and recovery log. Do not reboot if sideload reports a signature, assertion, block-image or boot write failure. On success, return to the main recovery menu and select `Reboot system now` only after checking that recovery remains reachable and no unexpected partition was targeted.

## First-boot validation

Allow a longer first boot than an ordinary restart. If the device reaches the Lineage setup UI, verify cold boot, reboot, power-off/charging behavior, display, touch, keys, Wi-Fi, Bluetooth, audio input/output, cameras, sensors, storage, USB/ADB, SIM detection, mobile data, calls, SMS, GPS, NFC and fingerprint before describing the ROM as usable. Preserve logs and distinguish untested features from failures.

The first installation completed successfully under explicit authorization. Recovery recorded successful data/cache formatting, updater status `1` and `Install complete`; boot and recovery prefix hashes and the installed system properties passed read-only checks before reboot. Subsequent hardware validation results, including the later Wi-Fi and GNSS repair builds, are recorded in `docs/test-matrix.md`.

## Recovery and rollback limits

The current stock boot partition was backed up outside Git as a complete 33,554,432-byte image with SHA-256 `1ab3c211b4c03bc88b92d685ff631e265064f78155360706faa131f9706acc29`. Restoring only that boot image after system has been replaced would create a mismatched stock-boot/Lineage-system combination and is not a complete rollback.

A matching complete CSA1 CHC factory archive is now stored outside Git and passes integrity checks, but a full stock restoration has not been rehearsed and would write a much broader partition set. It must not be treated as an automatic fallback or flashed without a separate exact plan and authorization. For a failed GNSS candidate, the narrow project rollback remains the complete pre-update boot/system images retained outside Git or a reviewed prior Lineage ZIP through the verified recovery. The TWRP backup restores recovery tooling only; it does not restore stock system, bootloader, modem or user data.
