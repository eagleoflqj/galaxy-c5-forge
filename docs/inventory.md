# P0 source and input inventory

Last updated: 2026-10-04

This inventory distinguishes verified online sources, source-derived candidate parameters, and facts verified from a physical device or stock package. The designated device and matching CSA1 CHC stock package have now been verified as evidence, but possession of the package does not itself authorize or validate full-package flashing.

## Current local inputs

| Item | Result | Evidence or notes |
| --- | --- | --- |
| Orchestration repository | Present as a local Git repository; initial commit contains the Apache-2.0 license and the project baseline is a separate commit; no remote | This repository worktree |
| Android source worktree | Synchronized from the pinned manifest with this repository's tracked patches applied; expected device-tree/recovery modifications and generated external vendor files make the worktree non-clean | `/mnt/storage/android/galaxy-c5-lineage-16`; see `manifests/locked/lineage-16.0.xml`; reproduce with `scripts/apply-patches.sh`, `scripts/import-stock-gnss.sh`, and `scripts/import-stock-sensors.sh` |
| Stock firmware/boot/recovery/PIT | Matching complete `C5000ZCU1CSA1` CHC firmware was supplied and passed integrity checks; the original boot and live-device PIT were exported read-only | Original ZIP is 2,247,133,405 bytes with SHA-256 `36946cd31d1817fb3a6d9e7b5fb4f2072df3dce5169eb644d86446801a860dc9`; PIT has 42 entries; all remain outside Git; recovery had been replaced by TWRP and was later replaced with the verified Lineage recovery |
| Historical ROM/third-party recovery | TWRP 3.2.3-0 was installed and bootable | Its recovery-partition image was exported read-only and hashed before replacement |
| Test device | Connected and investigated through sanitized read-only checks in TWRP, Lineage recovery, and Android | Hardware confirms `SM-C5000`, `c5lte`, msm8952, and bootloader `C5000ZCU1CSA1`; serial is not committed |
| Sensitive device data | Not collected | No IMEI, EFS, modemst, account, or user files were read; temporary ADB public-key files were removed |

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

- The [Samsung official SM-C5000 update record](https://doc.samsungmobile.com/SM-C5000/000041170406/eng.html) is accessible and lists `C5000ZHU1BRA1` (Android 7.0, 2018-01-19), `C5000ZHU1CRJ5` (Android 8.0, 2018-12-06), and `C5000ZHU1CSA1` (Android 8.0, 2019-02-01). It proves that these public versions existed but does not itself provide firmware images or source correspondence.
- The [Samsung Open Source](https://opensource.samsung.com/) portal exists, but command-line access returned HTTP 403 during this investigation, so the corresponding SM-C5000 source package has not been acquired or verified.
- The candidate [4PDA topic](https://4pda.to/forum/index.php?showtopic=743761) returned HTTP 403 during this investigation; inaccessible attachments or post contents were not treated as evidence.

## Verified CSA1 stock input

The user-provided `SAMFW.COM_SM-C5000_CHC_C5000ZCU1CSA1_fac.zip` passes ZIP integrity testing. It is 2,247,133,405 bytes with MD5 `887ed433bb432840ba3a9b341b215295` and SHA-256 `36946cd31d1817fb3a6d9e7b5fb4f2072df3dce5169eb644d86446801a860dc9`. Its AP member is `AP_C5000ZCU1CSA1_CL14615209_QB21439331_REV00_user_low_ship_MULTI_CERT_meta.tar.md5`.

The raw stock system ext4 image extracted outside Git is 3,196,059,648 bytes with SHA-256 `1a113c05150d765a5e8142eac2b3d4bb29b2eecbd776fd37092a6338678f50b6`. The matching `boot.img` remains separately in a restricted directory with SHA-256 `61fb897ee98bdd1995362f7c877cb1c37b2e7cfd09422599dad3b10529fc4bbd`; its initrd SHA-256 is `dfea9868054fd0ab8b618c22d325fc436d738787cf13fa3a695634812bcd4022`.

The GNSS evidence directory built from stock system contains 75 files, 45 of which form the hash-pinned import set for the candidate ROM. The sensor-stack import set contains the exact 28,476-byte `ram_patch.fw` and 120,508-byte legacy HAL. Original firmware, unpacked images, proprietary binaries, and generated vendor directories remain outside Git. `scripts/import-stock-gnss.sh` and `scripts/import-stock-sensors.sh` accept only the exact system image above and verify every imported SHA-256 before writing the external Android worktree.

## Remaining input boundaries

No additional firmware or device input is missing for the completed P0–P4 LineageOS 16 bring-up milestone. The exact GNSS and sensor-stack builds were installed under separate authorizations and passed their documented functional scopes.

The following remain outside that closed milestone:

1. A rehearsed full-stock restoration procedure and any authorization to write the complete factory package.
2. P5 inputs and execution for an independent clean rebuild, byte-level reproducibility investigation, and CI.
3. P6 source and test work for any modern-Android target.

The verified factory package is rollback evidence, not blanket authorization to flash stock partitions.
