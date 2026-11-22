# Galaxy C5 Forge

Galaxy C5 Forge brings LineageOS 17.1 and Linux 4.9 to the Samsung Galaxy C5 SM-C5000 (c5lte). Testing used a Rev09 handset with the CSA1 bootloader and no SIM installed.

## Choosing an image

The Linux 4.9 build uses two partition images. The filenames carry the November 21, 2026 rebuild date. Verify each file before use.

| File | Bytes | SHA-256 |
|---|---:|---|
| `lineage-17.1-20261121-c5lte-boot.img` | 33,554,432 | `f7f70aa7ed89516a6698a26a4bb8dc5adb28d78f76dfc343e0bf6381e3f92357` |
| `lineage-17.1-20261121-c5lte-system.img.gz` | 732,015,303 | `69806ab807a215acf8a67e4d48559b400025f8a970ff674fc604fcbf02209263` |

Decompress the SYSTEM image with `gzip -dk lineage-17.1-20261121-c5lte-system.img.gz`. The resulting raw image is 3,196,059,648 bytes and its SHA-256 is `f69387de277bca74c7a013f7402dc5bdb0087fdd7d53842de62db0df381745b2`.

Download the BOOT and compressed SYSTEM images from [the November 21 release](https://github.com/eagleoflqj/galaxy-c5-forge/releases/tag/17.1.20261121). A generic LineageOS 17.1 build for c5lte selects the older device kernel. Using that build alone does not produce the pair above.

The [kernel build guide](docs/kernel-build.md) gives the source revisions, firmware inputs and commands for rebuilding the kernel and full BOOT. The [SYSTEM build notes](docs/system-build.md) identify the Android baseline and final component changes.

## Device support

The acceptance run covered boot, shutdown and charging, fingerprint unlock, display and touch, rotation and brightness, front and rear cameras, positioning, speaker, wired and Bluetooth audio, recording, NFC, vibration and notification lights. Ordinary USB functions were tested. The exact USB accessory audio coverage is limited to the verified interface and descriptors.

Cellular service is outside the no-SIM acceptance scope. RMNET data and flow control and the legacy previous-boot log path have offline evidence only. Physical USB host operation and removable microSD were skipped because the equipment was unavailable. Fast charging depends on the charger and cable. With the same tested USB-A cable, both kernels charged without negotiating 9 V.

The tested handset ran with SELinux enforcing. Strict build-time neverallow checks rejected both the baseline and candidate vendor policy. The accepted precompiled policy matches the separately recorded build output. Passing a strict policy build remains an open item.

## Recovery

Verify the exact device model, bootloader and availability of Samsung Download Mode before a device trial. Keep a verified recovery image and the original BOOT and SYSTEM backups for that handset on another disk. The tested installation preserved RECOVERY and user data. A full-partition hash and a compact image hash refer to different byte ranges. Match the format and size before comparing.

## Source licenses

The orchestration code uses Apache-2.0. Kernel code and Android changes retain their upstream notices and applicable licenses. Samsung firmware and proprietary Android components are supplied separately by the builder.
