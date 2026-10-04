# P0 source and input inventory

Last updated: 2026-10-03

This inventory distinguishes verified online sources, source-derived candidate parameters, and facts verified from a physical device or stock package. No stock firmware package or connected device is currently available, so source declarations cannot be treated as production-hardware facts.

## Current local inputs

| Item | Result | Evidence or notes |
| --- | --- | --- |
| Orchestration repository | Present as a local Git repository; first commit is an Apache-2.0-licensed project baseline; no remote | This repository worktree |
| Android source worktree | Synchronized with a clean worktree | `/mnt/storage/android/galaxy-c5-lineage-16`; see `manifests/locked/lineage-16.0.xml` for the fully resolved manifest |
| Stock firmware/boot/recovery/PIT | Not provided | No image, archive, or PIT exists in the repository |
| Historical ROM/third-party recovery | Not provided | Only public source branches were investigated |
| Test device | Not connected | `adb devices -l` is empty and USB enumeration shows no Samsung device |
| Sensitive device data | Not collected | No serial, IMEI, EFS, or user data was read |

## Verified public source

The SHAs below are full object IDs queried on 2026-10-03. The development manifest pins SHAs and does not depend on branches continuing to reference the same objects.

### Device tree

Source: [vm03/android_device_samsung_c5lte](https://github.com/vm03/android_device_samsung_c5lte)

- Repository default branch: `master`; no tags; GitHub identifies no repository-level license.
- Branches and tips:

| Branch | Commit |
| --- | --- |
| `cm-13.0-twrp` | `c7dbde41e56c6c39c27c5262d03563bdf64508c2` |
| `cm-14.1` | `ab81ea5448ad10e268835f3029f5491d7e0dd78b` |
| `cm-14.1-shamrock1` | `f5ef6a251a9b2db0524174e4681c1c440303c3a8` |
| `cm-14.1-twrp` | `d4702c4d10b5934c19c8ab8cea64523c8f3d9c78` |
| `lineage-15.1` | `786d32b0426c581dc072d389f75c97a039fd7e5a` |
| `lineage-15.1-twrp` | `b2b0c457d129c6d3293495270812b2bc5f027134` |
| `lineage-16.0` | `5a5ee3ae766dfda573f6512223fa724279bafdab` |
| `master` | `d06541907585d60db217791b782ea6c96bfda9ab` |

- The public-fork check found only `moonlightly/android_device_samsung_c5lte`; its visible branches are from 2017 and contain no independent LineageOS 16 implementation newer than the table above.
- The `lineage-15.1-twrp` branch contains prebuilt crypto, F2FS, and QSEE-related binaries of unclear origin and licensing, so it remains an analysis alternative rather than the primary recovery baseline.

### Kernel

Source: [vm03/android_kernel_samsung_c5lte](https://github.com/vm03/android_kernel_samsung_c5lte)

- Default branch: `C5000ZCU1API2`; no tags; no public forks.
- The source contains GPL-2.0 `COPYING`, but external firmware and separately licensed files in the repository still require individual review.
- Branches and tips:

| Branch | Commit |
| --- | --- |
| `C5000ZCU1API2` | `9cbe0edcdfaa195a57a39a027afff4fd720672c6` |
| `C5000ZHU1BRA1` | `b424e770d932ad3805157fa6c0d703ad937a3927` |
| `C5000ZHU1BRA1-build` | `4283c96fddccbda67bf82bad48342ebb253f71d1` |
| `C5000ZHU1BRA1-build-aarch64` | `94631c41e794c7ba89f04ac74df9a98e89fd2e2b` |
| `cm-14.1` | `9a735eb2b1f32cd650329f1c6622815ab62d3d37` |
| `cm-14.1-old` | `196deba8b9381a183296451468dfa1caccf7620f` |
| `cm-14.1-old1` | `2aabccf3e58974d5b384fd4bc49fc19a870520cd` |
| `cm-14.1-old2` | `5c40f76a52a8bfa6da48409a3c5b94c22f3120c0` |
| `lineage-15.1` | `d6f647ee32b0bb384933d97ce1b1b49931fa4532` |
| `lineage-16.0` | `71b335bed497bbe8b9754c9c986801a66212ce4f` |

- `lineage-16.0` declares Linux `3.10.108`; earlier stock/15.1 clues indicate `3.10.84`.
- The history includes an import of `C5000ZCU1API2` and an update to `C5000ZHU1BRA1`, but no Samsung official source package is currently available for a file-by-file completeness and correspondence check.

### Proprietary vendor

Source: [vm03/proprietary_vendor_samsung_c5lte](https://github.com/vm03/proprietary_vendor_samsung_c5lte)

| Branch | Commit |
| --- | --- |
| `lineage-15.1` | `0a6a4da28721cfd034c3077eed096a1a9efc765b` |
| `lineage-16.0` | `ada0ddb036c256f43f32593ecf7a2593b7a8298c` |

- No tags and no repository-level `LICENSE`, `NOTICE`, or `COPYING`; public redistribution authorization is unconfirmed.
- The GitHub tree API shows 557 files and approximately 182,155,620 total blob bytes on `lineage-16.0`. This is a remote-tree inventory, not a local hash/provenance audit.
- Relative to 15.1, the 16.0 branch adds QTI Bluetooth, a camera ISP blob modified for Pie mutex restrictions, and telephony common from Tissot. Its history also contains Sony `suzu` clues, so it is not a provable set uniformly extracted from one known SM-C5000 stock package.

### Upstream baseline

| Project | Branch/commit | Purpose |
| --- | --- | --- |
| [LineageOS/android](https://github.com/LineageOS/android) | `lineage-16.0` / `e56f933c20b428410d1c53f41e6c0986880cb362` | Top-level manifest, whose default manifest is based on AOSP `android-9.0.0_r46` |
| [LineageOS/android_hardware_samsung](https://github.com/LineageOS/android_hardware_samsung) | `lineage-16.0` / `871d3f4777603ff6c33a021fc38c9b2265c54f83` | Samsung common hardware source |
| [LineageOS/android_bootable_recovery](https://github.com/LineageOS/android_bootable_recovery) | `lineage-16.0` / `3492c6b5259651dabb6ae2c65ae2633fd5bf46d2` | Standard recovery resolved by the top-level manifest |
| [LineageOS/android_device_qcom_common](https://github.com/LineageOS/android_device_qcom_common) | `lineage-15.1` / `6e48da0dd09ba3ccd7c84a147aed502ca9429dc5` | Legacy dependency declared by the device tree; no 16.0 branch |

`device/qcom/common` appears only in the candidate device tree's `lineage.dependencies`; no direct reference was found in source or build configuration. The current local manifest does not introduce the mismatched 15.1 project. If a build proves it necessary, handle the specific failure instead of mixing branches preemptively.

## Stock and community sources

- The [Samsung official SM-C5000 update record](https://doc.samsungmobile.com/SM-C5000/000041170406/eng.html) is accessible and lists `C5000ZHU1BRA1` (Android 7.0, 2018-01-19), `C5000ZHU1CRJ5` (Android 8.0, 2018-12-06), and `C5000ZHU1CSA1` (Android 8.0, 2019-02-01). It proves that these public versions existed but does not provide the complete firmware images or source correspondence required by this project.
- The [Samsung Open Source](https://opensource.samsung.com/) portal exists, but command-line access returned HTTP 403 during this investigation, so the corresponding SM-C5000 source package has not been acquired or verified.
- The candidate [4PDA topic](https://4pda.to/forum/index.php?showtopic=743761) returned HTTP 403 during this investigation; inaccessible attachments or post contents were not treated as evidence.

## Minimum genuinely missing inputs

No additional input is required to build a software-side candidate. Advancing P1/P2 from “candidate” to “verified” requires at least:

1. A complete SM-C5000 stock firmware package for a specific region and version, preserving the original file and recording its source, acquisition date, and SHA-256; prefer a package matching the test device's current bootloader/firmware.
2. One explicitly designated SM-C5000 on which read-only ADB and system-information collection are allowed. If the system cannot enable ADB, establish a separate safe acquisition path.
3. Before any boot/recovery write, the device's current version, OEM/FRP state, and a recoverable stock package, plus separate authorization for the exact image, hash, partition, and recovery plan.

Neither the stock package nor the physical device blocks current source synchronization, static audit, or compilation.
