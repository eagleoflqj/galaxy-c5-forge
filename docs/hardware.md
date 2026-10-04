# SM-C5000 hardware and partition fact table

Last updated: 2026-10-03

This table uses three states: **officially confirmed**, **source-derived candidate**, and **unverified**. No live-device data, PIT, or stock image is currently available, so every value that affects flashing safety must still be cross-checked against firmware and the designated device before any write.

## Identity and software versions

| Item | Current value | Status and evidence |
| --- | --- | --- |
| Retail model | Galaxy C5 `SM-C5000` | Officially confirmed by Samsung update records |
| Build product name | `c5lte` | Source-derived candidate from the Lineage product definition and lunch name |
| Samsung stock product | `c5ltechn` / `c5ltezh` | Source-derived candidate from fingerprints and properties; not confirmed on a device |
| Lineage product | `lineage_c5lte-userdebug` | Build target confirmed by source; does not imply working hardware |
| Stock Android | At least Android 7.0 and 8.0 exist | Officially confirmed by BRA1, CRJ5, and CSA1 update records |
| Candidate stock fingerprint | `samsung/c5ltezh/c5ltechn:7.0/NRD90M/C5000ZHU1BRA1:user/release-keys` | Device-tree candidate not verified against a stock system |
| Recovery bootloader assertion | `C5000ZHU1CRJ5` | Candidate from device-tree `board-info.txt`; whether a single-version restriction is correct is unverified |

The BRA1 fingerprint, BRA1 kernel origin, and CRJ5 bootloader requirement do not come from one stock release. This mixed-baseline risk requires firmware or live-device evidence and cannot be resolved by existing strings in source.

## SoC, architecture, and kernel

| Item | Current candidate | Evidence or validation gap |
| --- | --- | --- |
| SoC/BSP | Qualcomm `msm8952` | `BoardConfig.mk`, defconfig, and DTS; live SoC information has not been read |
| Primary ABI | ARM64 | `TARGET_ARCH := arm64` and ARM64 kernel defconfig |
| Compatible ABI | 32-bit ARM | `TARGET_2ND_ARCH := arm` and kernel compatibility configuration |
| Kernel | Linux 3.10.108 | Pinned Lineage 16 kernel branch; not compared with the stock boot kernel |
| defconfig | `arch/arm64/configs/c5lte_defconfig` | Confirmed in the kernel tree |
| Device configuration | `CONFIG_ARCH_MSM8952=y`, `CONFIG_SEC_C5LTE_PROJECT=y`, `CONFIG_MACH_C5LTE_CHN_OPEN=y` | Confirmed in the kernel tree |
| DTS | `arch/arm/boot/dts/samsung/msm8952/msm8952-sec-c5lte-chn-hk-r09.dts` | Confirmed in the kernel tree |
| DTS identity | model `Samsung C5LTE CHN OPEN PROJECT Rev09`; msm-id `0x108`; board-id `<0x8 0x9>` | Source-derived candidate not compared with the actual DTB or hardware revision |

## boot/recovery image parameters

| Item | Source-derived candidate | Status |
| --- | --- | --- |
| base | `0x80000000` | Declared by the device tree; not verified from a stock boot header |
| page size | `2048` | Declared by the device tree; not verified from an image |
| tags offset | `0x01e00000` | Declared by the device tree; not verified from an image |
| ramdisk offset | `0x02000000` | Declared by the device tree; not verified from an image |
| kernel image | `Image.gz` | Candidate from kernel/device build configuration |
| DT | Separate DT | `BOARD_KERNEL_SEPARATED_DT := true`; actual packaging remains unverified |
| mkbootimg | Samsung-specific `hardware/samsung/mkbootimg.mk` | Source-path candidate pending the first build and image parsing |
| cmdline | `console=null androidboot.hardware=qcom user_debug=30 msm_rtb.filter=0x237 ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci lpm_levels.sleep_disabled=1` | Declared by the device tree; not verified from a stock image |

The first Lineage recovery build actually produced a header with kernel `0x80008000`, ramdisk `0x81000000`, and tags `0x80000100`. This proves that the current Android 9 packaging command did not use the ramdisk/tags offsets declared by the device tree; it is not evidence of stock parameters. See `docs/build.md` for the difference and artifact hash. Do not guess which address set will boot until a stock boot/recovery header is available.

## Partition candidates

Capacities come from device-tree `BoardConfig.mk`; names and filesystems come from recovery/system fstab. They are not measured PIT or block-device results.

| Partition | Candidate capacity in bytes | Notes |
| --- | ---: | --- |
| `boot` | 33,554,432 | Candidate by-name path |
| `recovery` | 33,554,432 | The first build artifact must be smaller than this candidate limit; revalidate before writing |
| `cache` | 209,715,200 | ext4 candidate |
| `persist` | 33,554,432 | ext4 candidate excluded from routine writes |
| `system` | 3,196,059,648 | ext4 candidate |
| `userdata` | 27,518,808,064 | ext4/F2FS candidate with candidate legacy footer encryption |

The candidate flash block size is `131072`. fstab also references `fota`/`misc`, `modem`, `apnhlos`, `config`/FRP, `efs`, and `dsp`, and it declares no A/B slots, separate `vendor`, or `dtbo`. Absence from these declarations does not replace live-device verification. `efs`, modem, bootloader, PIT, and repartitioning are explicitly outside automated write scope.

## Peripheral clues, all unverified

The device-tree README claims 4 GiB RAM, 32/64 GiB storage, a 5.2-inch 1080p AMOLED panel, Snapdragon 617/Adreno 405, S5K3P3SX/S5K4H5YC cameras, BHI160, TFA9897, RT5659, YAS539, Synaptics VFS7xxx, PN547, TMD3700, ZT7538, and other components. These details are currently only source-navigation clues, not support commitments, and they do not justify using blobs from another model.

## Evidence required for P1/P2

- Stock boot/recovery header, segments, compression, DTB, cmdline, trailer/signature, and SHA-256.
- ABI, HAL, service, SELinux, permission, and blob-hash inventory for stock system/vendor when present.
- Sanitized `getprop`, kernel version, by-name links, block-device capacities, and mounts from the designated device, plus PIT analysis only when it can be acquired safely.
- Consistency among the bootloader, OEM/FRP state, current firmware, and a recoverable stock package.
- Actual recovery boot, display/key/touch, ADB, partition discovery, and stock-return results on hardware.

Until this evidence is complete, any recovery artifact may be labeled only “built, not verified on device.”
