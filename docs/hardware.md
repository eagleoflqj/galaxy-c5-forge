# SM-C5000 hardware and partition fact table

Last updated: 2026-10-04

This table uses four states: **officially confirmed**, **measured on device**, **verified from a stock package**, and **source-derived candidate**. On 2026-10-03, the designated device was investigated from TWRP 3.2.3-0, its then-current stock boot and TWRP recovery were exported, and its PIT was subsequently exported read-only from Download Mode. On 2026-10-04, the matching complete `C5000ZCU1CSA1` CHC stock package was verified and parsed. The stock package was not written to the phone.

## Identity and software versions

| Item | Current value | Status and evidence |
| --- | --- | --- |
| Retail model | Galaxy C5 `SM-C5000` | Officially confirmed by Samsung update records |
| Build product name | `c5lte` | Source-derived candidate from the Lineage product definition and lunch name |
| Samsung stock product | `c5pltechn` / `c5pltezc` | Measured on device from the then-current Android 8.0 CSA1 release-key system properties |
| Lineage product | `lineage_c5lte-userdebug` | Build target confirmed by source; does not imply working hardware |
| Stock Android before Lineage installation | Android 8.0 with security patch 2019-01-01 | Measured on device from system build `R16NW.C5000ZCU1CSA1` |
| Current Android | LineageOS 16.0 / Android 9 with security patch 2022-01-05 | Measured on device; the currently installed CSA1 sensor-stack ROM passes system, Wi-Fi, GNSS, compass, step-counter, proximity-sensor, and wired-headset-output tests |
| Candidate stock fingerprint | `samsung/c5ltezh/c5ltechn:7.0/NRD90M/C5000ZHU1BRA1:user/release-keys` | Device-tree candidate not verified against the stock system |
| Live bootloader | `C5000ZCU1CSA1` | Measured on device and inconsistent with the device tree's single-value `C5000ZHU1CRJ5` assertion |

The BRA1 fingerprint, BRA1 kernel origin, and CRJ5 bootloader requirement do not come from one stock release, while the designated device is the Chinese-region CSA1 release. A tracked patch preserves CRJ5 while also allowing the measured CSA1 bootloader, so the final ROM ZIP no longer rejects this device. Correcting the assertion does not eliminate the mixed-baseline compatibility risk.

## SoC, architecture, and kernel

| Item | Current candidate | Evidence or validation gap |
| --- | --- | --- |
| SoC/BSP | Qualcomm `msm8952` | Live `ro.board.platform` agrees with source |
| Primary ABI | ARM64 | `TARGET_ARCH := arm64` and ARM64 kernel defconfig |
| Compatible ABI | 32-bit ARM | `TARGET_2ND_ARCH := arm` and kernel compatibility configuration |
| Kernel | Linux 3.10.108 | Pinned Lineage 16 kernel branch; final recovery boots and reports `3.10.108-g71b335bed49`; matching stock boot was extracted and uses the same boot-address layout, but kernel contents have not been compared item by item |
| defconfig | `arch/arm64/configs/c5lte_defconfig` | Confirmed in the kernel tree |
| Device configuration | `CONFIG_ARCH_MSM8952=y`, `CONFIG_SEC_C5LTE_PROJECT=y`, `CONFIG_MACH_C5LTE_CHN_OPEN=y` | Confirmed in the kernel tree |
| DTS | `arch/arm/boot/dts/samsung/msm8952/msm8952-sec-c5lte-chn-hk-r09.dts` | Confirmed in the kernel tree |
| DTS identity | model `Samsung C5LTE CHN OPEN PROJECT Rev09`; msm-id `0x108`; board-id `<0x8 0x9>` | Source-derived candidate not compared with the actual DTB or hardware revision |

## boot/recovery image parameters

| Item | Source-derived candidate | Status |
| --- | --- | --- |
| base | `0x80000000` | Verified from the live-device boot header |
| page size | `2048` | Verified from the live boot and TWRP recovery headers |
| tags offset | `0x01e00000` | Verified because live boot tags are at `0x81e00000` relative to the base |
| ramdisk offset | `0x02000000` | Verified because the live boot ramdisk is at `0x82000000` relative to the base |
| kernel image | `Image.gz` | Candidate from kernel/device build configuration |
| DT | Separate DT | `BOARD_KERNEL_SEPARATED_DT := true`; final recovery packaging and hardware boot are verified |
| mkbootimg | Samsung-specific `hardware/samsung/mkbootimg.mk` | Built, parsed, and verified by booting on hardware |
| cmdline | `console=null androidboot.hardware=qcom user_debug=30 msm_rtb.filter=0x237 ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci lpm_levels.sleep_disabled=1` | Final recovery boots with it; matching stock boot contains the same hardware, RTB, EHCI, bootdevice, and LPM parameters but omits Lineage `user_debug=30` |

The live-device boot header uses kernel `0x80008000`, ramdisk `0x82000000`, and tags `0x81e00000`, confirming the device-tree offsets. Its complete 32 MiB partition image has SHA-256 `1ab3c211b4c03bc88b92d685ff631e265064f78155360706faa131f9706acc29`. The backed-up, bootable TWRP recovery uses kernel `0x80008000`, ramdisk `0x81000000`, and tags `0x80000100`. Final Lineage recovery uses the live-boot-verified `0x80008000`, `0x82000000`, and `0x81e00000` addresses; it was successfully flashed and booted on the designated device. Its 16,234,512-byte image has SHA-256 `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`.

The matching stock package's 15,816,976-byte `boot.img` has SHA-256 `61fb897ee98bdd1995362f7c877cb1c37b2e7cfd09422599dad3b10529fc4bbd` and likewise uses page size 2,048, kernel `0x80008000`, ramdisk `0x82000000`, and tags `0x81e00000`. This independently confirms the device-tree boot addresses but does not mean the Lineage and stock kernels are byte-identical.

## Partitions

Names, block-device links, and capacities were verified from the designated device's `/dev/block/bootdevice/by-name`, `/proc/partitions`, and `blockdev --getsize64`, then cross-checked against the critical partitions in a 42-entry PIT exported read-only with Heimdall. The PIT is 8,192 bytes with SHA-256 `657d80ed96e111e857ff4fd9062fbb58aaf466e1248f36ce6fa8f61de609b1b0` and is retained in a mode-600 evidence directory outside Git.

| Partition | Measured capacity in bytes | Notes |
| --- | ---: | --- |
| `boot` | 33,554,432 | `/dev/block/mmcblk0p18`; the then-current image was exported read-only |
| `recovery` | 33,554,432 | `/dev/block/mmcblk0p19`; PIT Entry #18, Identifier 19, offset 176128, count 65536; final Lineage recovery was written and verified under single-partition authorization |
| `cache` | 209,715,200 | `/dev/block/mmcblk0p37`; currently ext4 |
| `persist` | 33,554,432 | `/dev/block/mmcblk0p25`; excluded from routine writes |
| `system` | 3,196,059,648 | `/dev/block/mmcblk0p36`; CSA1 properties were verified through a `ro,noload` mount |
| `userdata` | 58,787,344,384 | `/dev/block/mmcblk0p38`; currently ext4, with legacy footer encryption declared by TWRP fstab |

The candidate flash block size is `131072`. The first Lineage recovery's failed `fota` stat occurred while the entire `/dev/block/bootdevice` link hierarchy was absent and does not disprove a FOTA partition. After the third candidate restored the parent links, cache mounted and produced persistent logs, but the temporary `misc` path failed. The PIT contains no `MISC`; the working TWRP ramdisk and c5lte SELinux rules both map `/misc` to `by-name/fota`, so the final patch preserves bootdevice initialization and restores `fota`. Final recovery root ADB resolves `boot`, `recovery`, `fota`, `system`, `cache`, and `userdata` to p18, p19, p21, p36, p37, and p38 respectively, and boot plus sideload round-trip logs contain no BCB, FOTA, data, or cache errors. fstab also references `modem`, `apnhlos`, `config`/FRP, `efs`, and `dsp`, and declares no A/B slots, separate `vendor`, or `dtbo`. Absence from these declarations does not replace live-device verification. `efs`, modem, bootloader, PIT, and repartitioning are explicitly outside automated write scope.

## Peripheral clues from source

The device-tree README claims 4 GiB RAM, 32/64 GiB storage, a 5.2-inch 1080p AMOLED panel, Snapdragon 617/Adreno 405, S5K3P3SX/S5K4H5YC cameras, BHI160, TFA9897, RT5659, YAS539, Synaptics VFS7xxx, PN547, TMD3700, ZT7538, and other components. These details are source-navigation clues rather than support commitments and do not justify using blobs from another model.

## LineageOS 16 hardware results

| Item | Status | Evidence and scope |
| --- | --- | --- |
| System boot, display, touch, launcher | PASS | Initial installation completed setup and reached the launcher; this does not prove graphics acceleration or long-term stability |
| Normal reboot | PASS | A controlled Android reboot completed successfully; full power-off, offline charging, and cold boot were later tested separately |
| Android USB ADB | PASS | The userdebug build connects normally; after the user enabled `Root access: ADB only`, root ADB works while SELinux remains enforcing |
| Wi-Fi | PASS | After adding `adsp.b15`, ADSP/WCNSS start successfully; network discovery, user association, IPv4/default route/IP/DNS traffic, and automatic reassociation after a normal reboot all pass |
| GPS/location | PASS | The second CSA1 Samsung/QTI stack is installed; SatStat detected 17 satellites, Organic Maps found the correct location, sanitized metrics recorded 162 location reports and three TTFF reports with a mean of about 5.67 seconds, GNSS processes remained stable, and there were no linker or fatal-process errors; no coordinates were retained |
| Compass heading | PASS | The user confirmed correct map compass direction; the YAS539 magnetic-field hardware self-test had also passed |
| Step counter | PASS | After installing the exact matching CSA1 `ram_patch.fw` and sensor HAL, Paseo counts normally; SensorService recorded a sequence increasing from 0 to 48, BHY kernel logs reported matching positive step differences, and the old sensor type 59/60 invalid-FIFO and sequential-processing errors did not recur |
| Proximity sensor | PASS | The user confirmed correct cover/uncover transitions with the phyphox Proximity timer experiment |
| Wired-headset output | PASS | The user confirmed normal audio through a connected wired headset; wired-headset microphone behavior was not claimed or inferred |

Wi-Fi failure logs originally showed ADSP loading from shared reloc CMA at `0x8c600000` through `0x8de00000`, successfully reading `adsp.b00` through `adsp.b14`, then exiting because `adsp.b15` was absent. The modem firmware partition contains that 131,444-byte file, whose size exactly matches segment 15 in `adsp.mdt`; it was absent only from the device tree's `ADSP_IMAGES` list. The ADSP secure-load error path has no `deinit_image` callback, after which WCNSS obtained the same physical base and returned `-2` from `PAS_MEM_SETUP`. ADSP's 24 MiB plus WCNSS's 7 MiB aligned to 1 MiB fit the shared 31 MiB region exactly, and the Rev09 DT extracted from CSA1 stock boot matches the current WCNSS/reserved-memory definitions. The minimum fix adds only `/system/etc/firmware/adsp.b15 -> /firmware-modem/image/adsp.b15` and does not modify the kernel, SCM, DT, or firmware partition.

That minimum fix was subsequently installed and verified on hardware. New boot logs explicitly read `adsp.b15`, ADSP exits reset successfully, and WCNSS loads from the immediately following `0x8de00000` base, exits reset, and completes SMD/NV initialization. The former missing-`adsp.b15` and WCNSS memory-setup errors do not recur. The user confirmed that Android discovers and connects to nearby networks; sanitized checks confirm carrier, global IPv4, default route, IP traffic, and DNS traffic. Wi-Fi reassociation and traffic pass again after a normal reboot, SELinux remains enforcing, and the test retained no SSID, BSSID, password, or local address.

The second GNSS repair was later written to system/boot under explicit authorization while preserving userdata/cache. It replaces the failed `LD_PRELOAD` design with explicit rewritten dependency names. Android completed boot in about 37 seconds and the Samsung/QTI GNSS services remained stable. Functional testing confirmed 17 satellites, correct map location, and a working compass heading. A PeripheralManager registration-path AVC remains during startup, but it does not prevent location within the tested scope, so it is a non-blocking known warning and does not justify further flashing.

The step-counting failure had been isolated to the BHY custom-pedometer FIFO: the application and framework registered successfully and BHA250 detected movement, but mismatched firmware from the public vendor tree caused the kernel to reject sensor type 59/60 records. The exact CSA1 `ram_patch.fw` and sensor HAL were then built and installed as a matched pair and passed hardware testing. Agreement among Paseo, SensorService, and kernel counts proves the fix, and the old FIFO errors do not recur.

## Remaining evidence gaps

- A more complete content-level cross-check among stock boot, recovery, kernel, and corresponding Lineage components; stock boot extraction, hashes, and header-parameter validation are complete.
- An expanded ABI, HAL, service, SELinux, permission, and blob-provenance inventory for stock system; the GNSS and sensor-stack scopes have hash pinning and dependency analysis.
- A complete explanation of every PIT partition; the PIT, sanitized `getprop`, kernel version, by-name links, critical block-device capacities, and mounts have been collected.
- Consistency among the bootloader, OEM/FRP state, current firmware, and the recoverable stock package.
- Recovery hardware boot, display, touch, normal root ADB, sideload round trip, and partition discovery are verified. A complete stock-restore procedure has not been rehearsed and is not authorized.

Final recovery may be labeled “built and verified on the designated CSA1 device.” The installed LineageOS 16 sensor-stack build is verified only for the scopes recorded above; untested peripherals and complete stock restoration must not be inferred to pass.
