# Galaxy C5 Forge

Galaxy C5 Forge runs LineageOS 17.1 and 18.1 on the Samsung Galaxy C5 SM-C5000 (c5lte) with Linux 4.9. The device tests used a Rev09 handset with the CSA1 bootloader.

## LineageOS 18.1 for the SM-C5000

The build is `18.1-20261123-UNOFFICIAL-c5lte` (Android 11, Linux 4.9.337). BOOT and RECOVERY are separate compact images, and SYSTEM expands to a raw ext4 image. Upgrading BOOT and SYSTEM preserved the existing user data in the 17.1 upgrade trial. Install the matching Lineage Recovery image separately to upgrade RECOVERY. Back up both the phone and its original partitions before installing.

Download the three images and `SHA256SUMS` from the [18.1 release](https://github.com/eagleoflqj/galaxy-c5-forge/releases/tag/18.1.20261123). Run `sha256sum -c SHA256SUMS` in the download directory, then decompress SYSTEM with `gzip -dk lineage-18.1-20261123-c5lte-system.img.gz`.

| Uncompressed image | Bytes | SHA-256 |
|---|---:|---|
| BOOT | 11,737,104 | `2afea2c13b3e49277ef4ef48bf2dc6950284433a06618ddbd444f55c23366800` |
| SYSTEM | 3,196,059,648 | `ba4adde64662c74b8da5b603ad93231f70865fdffbf9b6ee96ef42300fffecb7` |
| RECOVERY | 19,652,624 | `c7db65ac506765b7b8eb33112a691f53845ae5ad0912f71df39b4b0951b9e9fa` |

To build the images, follow the [Android 11 build instructions](docs/system-build.md#lineageos-181-with-the-49-kernel). They use the pinned source manifest and a separately supplied CSA1 vendor tree. The build produces a sparse SYSTEM image. The instructions include the conversion to the raw format used by the table above.

### Tested features

| Area | Observed on the SM-C5000 |
|---|---|
| System and controls | Boot with SELinux enforcing, fingerprint unlock, all three navigation keys, Settings cold launch, screen wake, suspend on battery, vibration and the RGB notification light |
| Display and sensors | Portrait and landscape rotation, automatic brightness, proximity transitions and step counting |
| Camera and audio | Front and rear photos and video, rear HDR capture, speaker and microphone, wired headphones, Bluetooth media and two-way headset audio |
| Wireless | Wi-Fi gateway traffic, NFC tag discovery and delivery to a foreground app, and an outdoor GNSS fix in 16.8 seconds |
| USB and charging | ADB, MTP file transfer, PTP photo transfer, bidirectional MIDI, RNDIS traffic over USB, ordinary USB charging, and return from the off-mode charging screen with the power key |

USB accessory audio coverage reached interface negotiation and verification of the stereo 44.1 kHz, 16-bit PCM descriptors. Cellular calls and data, USB host peripherals, removable microSD and 9 V fast charging were skipped because the required equipment was unavailable.

## LineageOS 17.1 images

The published 17.1 build uses two partition images. The filenames carry the November 21, 2026 rebuild date. Verify each file before use.

| File | Bytes | SHA-256 |
|---|---:|---|
| `lineage-17.1-20261121-c5lte-boot.img` | 33,554,432 | `f7f70aa7ed89516a6698a26a4bb8dc5adb28d78f76dfc343e0bf6381e3f92357` |
| `lineage-17.1-20261121-c5lte-system.img.gz` | 732,015,303 | `69806ab807a215acf8a67e4d48559b400025f8a970ff674fc604fcbf02209263` |

Decompress the SYSTEM image with `gzip -dk lineage-17.1-20261121-c5lte-system.img.gz`. The resulting raw image is 3,196,059,648 bytes and its SHA-256 is `f69387de277bca74c7a013f7402dc5bdb0087fdd7d53842de62db0df381745b2`.

Download the BOOT and compressed SYSTEM images from [the November 21 release](https://github.com/eagleoflqj/galaxy-c5-forge/releases/tag/17.1.20261121). A generic LineageOS 17.1 build for c5lte selects the older device kernel. Using that build alone does not produce the pair above.

The [kernel build guide](docs/kernel-build.md) gives the source revisions, firmware inputs and commands for rebuilding the kernel and full BOOT. The [SYSTEM build notes](docs/system-build.md) identify the Android baseline and final component changes.

## LineageOS 17.1 device support

The acceptance run covered boot, shutdown and charging, fingerprint unlock, display and touch, rotation and brightness, front and rear cameras, positioning, speaker, wired and Bluetooth audio, recording, NFC, vibration and notification lights. Ordinary USB functions were tested. The exact USB accessory audio coverage is limited to the verified interface and descriptors.

Cellular service is outside the no-SIM acceptance scope. RMNET data and flow control and the legacy previous-boot log path have offline evidence only. Physical USB host operation and removable microSD were skipped because the equipment was unavailable. Fast charging depends on the charger and cable. With the same tested USB-A cable, both kernels charged without negotiating 9 V.

The tested handset ran with SELinux enforcing. Strict build-time neverallow checks rejected both the baseline and candidate vendor policy. The accepted precompiled policy matches the separately recorded build output. Passing a strict policy build remains an open item.

## Recovery

Verify the exact device model, bootloader and availability of Samsung Download Mode before a device trial. Keep a verified recovery image and the original BOOT and SYSTEM backups for that handset on another disk. The tested installation preserved RECOVERY and user data. A full-partition hash and a compact image hash refer to different byte ranges. Match the format and size before comparing.

## Source licenses

The orchestration code uses Apache-2.0. Kernel code and Android changes retain their upstream notices and applicable licenses. Samsung firmware and proprietary Android components are supplied separately by the builder.
