# P0 baseline selection

Last updated: 2026-10-03

## Decision

The initial software baseline is **LineageOS 16.0 / Android 9 with standard Lineage recovery**, using product target `lineage_c5lte-userdebug` and `recoveryimage` as the first build target.

This choice identifies only the buildable candidate with the fewest dependencies and most consistent public branches. It does not mean that the ROM has built successfully, recovery has booted, or any hardware function has been verified.

The pinned inputs are in `manifests/local/c5lte-lineage-16.xml`:

| Component | Pinned commit |
| --- | --- |
| Lineage top-level manifest | `e56f933c20b428410d1c53f41e6c0986880cb362` |
| c5lte device | `5a5ee3ae766dfda573f6512223fa724279bafdab` |
| c5lte kernel | `71b335bed497bbe8b9754c9c986801a66212ce4f` |
| c5lte proprietary vendor | `ada0ddb036c256f43f32593ecf7a2593b7a8298c` |
| Samsung common hardware | `871d3f4777603ff6c33a021fc38c9b2265c54f83` |

After source synchronization succeeds, `repo manifest -r` will generate `manifests/locked/lineage-16.0.xml` containing every upstream project SHA. Until then, the complete Android tree cannot be described as fully locked.

## Why this baseline

- The device, kernel, and vendor repositories all have a `lineage-16.0` branch, and the device tree registers the real `lineage_c5lte` product, so there is no need to invent a lunch target.
- The device branch already contains Android 9 compatibility commits, and the vendor branch contains Pie-specific changes, making this a suitable baseline for exposing real build failures.
- Standard recovery resolves from the same Lineage 16 source baseline, avoiding prebuilt recovery/crypto components of unclear origin from an old TWRP branch during the first iteration.
- Android 9 is closer to the existing 3.10 kernel and blob interfaces than a direct modern-Android port, making it appropriate for the minimum goal of validating the boot chain, ramdisk, partition descriptions, and ADB.

## Known risks and reservations

1. The device tree's BRA1 Android 7 fingerprint, the kernel's BRA1 origin, and the CRJ5 Android 8 bootloader assertion indicate a mixed-version baseline. No stock package currently proves that the combination matches completely.
2. The vendor repository has no explicit redistribution license, and its history shows blobs or modifications from other devices. It is currently used only as a public build input; release eligibility and true provenance remain unresolved.
3. Although the kernel repository includes GPL `COPYING`, it has not yet been reproducibly compared with a Samsung official source package or the actual stock boot kernel.
4. `lineage.dependencies` declares `LineageOS/android_device_qcom_common`, but that project has no Lineage 16 branch and the device source contains no direct use of it. Do not mix in 15.1 now; if the first build exposes a real dependency failure, handle it from the exact error and call site.
5. Partition capacities, boot-header values, and DT packaging are all source-derived candidates and are not strong enough to justify safe flashing.

## Retained alternatives

- **LineageOS 15.1 plus the TWRP device branch**: it has a more complete TWRP configuration and can serve as a comparison if standard recovery cannot provide required diagnostics, but its prebuilt crypto/F2FS/QSEE files have unclear licensing and origin and the branch is older, so it is not the first choice.
- **Standalone TWRP 9.0 baseline**: OmniROM recovery `android-9.0` and the TWRP minimal manifest `twrp-9.0` were identified, but this would introduce a second upstream combination and there is no evidence that it is smaller or more reliable than standard recovery from the same Lineage tree.
- **Prebuilt stock kernel**: after stock boot becomes available, it may help isolate kernel and ramdisk failures, but only as a diagnostic experiment; it cannot replace reproducible kernel source.

## Minimum execution path

1. Synchronize source from the pinned manifest in a clean work directory outside the repository and export the resolved manifest.
2. Build `recoveryimage` in the pinned Ubuntu 20.04 container and retain the complete log.
3. Check `recovery.img` size and SHA-256, then parse its header, DT, and ramdisk with tools matching the actual format. Compare only against source-derived candidates and do not treat agreement as stock verification.
4. After obtaining stock firmware, parse it read-only to generate repeatable P1 data and blob provenance.
5. After obtaining the designated device and operation-specific authorization, review the device/version/hash/capacity/recovery path before validating recovery boot. Do not perform this step yet.

## Phase exit condition

The P0 investigation deliverables are complete at the documentation level. Source synchronization and the first build are immediately executable software work. P2 passes only after recovery boots on a real device, ADB connects, and logs are retained. If only an image is produced, its status must remain “built, not verified.”
