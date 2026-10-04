# Build record

## First recovery build — 2026-10-03

Status: **compiled successfully; not tested on hardware**.

### Inputs and command

- Resolved manifest: `manifests/locked/lineage-16.0.xml`
- Manifest SHA-256: `97f3b77948bf7e86cf0550952e685669fdfc58ae7483e28271edc9aa94a3ae63`
- Container image ID: `sha256:7df314f3996641b191aab40af76aa90260f23ac3f212952fd112c25c902cb22d`
- Product: `lineage_c5lte-userdebug`
- Command:

```bash
./scripts/build.sh \
  --workdir /mnt/storage/android/galaxy-c5-lineage-16 \
  --target recovery \
  --jobs 16
```

- Reported build time: 4 minutes 35 seconds.
- Local full log: `logs/build-recovery-20261003T224814Z.log` (ignored by Git), 1,463,561 bytes, SHA-256 `e2c1e7e68f2f95be8c833bd1eb19950d11be2eeb5680e25504e4b2050ad0ed1a`.
- This was a first build in the newly synchronized worktree. It is not an independent clean rebuild and does not establish byte-for-byte reproducibility.

### Artifacts

| File | Size | SHA-256 |
| --- | ---: | --- |
| `recovery.img` | 16,236,560 | `d0d6b18ff371bca275a354c07b1f9528fe0259dc557e9439511002e0535d78f3` |
| `kernel` (`Image.gz`) | 9,020,061 | `21e80aeb2b38b77c00d09a194e1ebfce2bf54ca4c7ff6da9021331b5b051c29c` |
| `ramdisk-recovery.img` | 6,971,501 | `053986297151def0a59287e7d815315f08e4e0f3230103abdc2af6cd886888e2` |
| `dt.img` | 239,616 | `0b2e1df220eb2668b0f77b0adcc32930aef747e650c5b71be6cdb86a9d1b4b14` |

The recovery image is 17,317,872 bytes below the device-tree partition-size candidate of 33,554,432 bytes. That comparison is a build check only; the true device partition must still be verified from PIT/block data before any write.

### Read-only image inspection

`file`, `abootimg`, `binwalk`, gzip validation and cpio listing produced:

- Android boot image magic and 2,048-byte pages.
- Kernel size 9,020,061 and ramdisk size 6,971,501 bytes.
- Header addresses: kernel `0x80008000`, ramdisk `0x81000000`, tags `0x80000100`.
- `abootimg` ID: `f24d8a52 ead0d6a4 6e28b61a 15f31aaf db082f00 00000000 00000000 00000000`.
- Command line: `console=null androidboot.hardware=qcom user_debug=30 msm_rtb.filter=0x237 ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci lpm_levels.sleep_disabled=1 buildvariant=userdebug`.
- Qualcomm DT container v2 with one DTB; `binwalk` locates the DT container at `0xF41800` and the flattened tree at `0xF42000`.
- Valid gzip recovery ramdisk containing `init`, `recovery`, `adbd`, `toybox_static`, `prop.default` and `etc/recovery.fstab`.
- AArch64 static recovery and ADB executables for Android 28.
- Recovery properties include `ro.secure=1`, `ro.debuggable=1`, `ro.adb.secure=1`, and `persist.sys.usb.config=adb`.
- The final 16 bytes are Samsung's `SEANDROIDENFORCE` marker.

Re-run the structural and hash checks with:

```bash
./scripts/inspect-recovery.sh \
  --image /mnt/storage/android/galaxy-c5-lineage-16/out/target/product/c5lte/recovery.img \
  --partition-size 33554432 \
  --expected-sha256 d0d6b18ff371bca275a354c07b1f9528fe0259dc557e9439511002e0535d78f3
```

### Unresolved packaging findings

1. `board/kernel.mk` declares ramdisk offset `0x02000000` and tags offset `0x01e00000`, but the generated Ninja command passes only base and page size. The resulting header therefore uses mkbootimg defaults (`0x81000000` and `0x80000100`) instead of the declared offsets. Stock boot/recovery headers are required to determine the correct values before hardware testing.
2. `hardware/samsung/mkbootimg.mk` sets the Loki command to `echo` when `TARGET_NEEDS_LOKI` is false, then attempts to copy the nonexistent `.lok` output under `|| true`. This produces a visible `cp: cannot stat` message but leaves the original image intact and does not fail the build. No evidence currently indicates that SM-C5000 should use Loki.
3. `unpack_bootimg` from the Ubuntu 26.04 host fails to decode this legacy image header, while `file`, `abootimg` and `binwalk` parse it. Use format-matched tooling and compare against an actual stock image rather than assuming the newest unpacker is authoritative for this image.

No image has been flashed, booted, or tested. A successful compile does not resolve the header-offset discrepancy or prove that display, input, storage, USB, ADB, decryption, or reboot works on SM-C5000.

## First full ROM build — 2026-10-03/04

Status: **compiled successfully after one diagnosed source-materialization failure; not tested on hardware**.

### Command and failure diagnosis

```bash
./scripts/build.sh \
  --workdir /mnt/storage/android/galaxy-c5-lineage-16 \
  --target rom \
  --jobs 16
```

The first invocation compiled 83,131 of 83,828 Ninja actions and failed after 1 hour 2 minutes 51 seconds while signing the WebView APK. The input at `external/chromium-webview/prebuilt/arm64/webview.apk` was a 134-byte Git LFS pointer rather than a ZIP. Its pointer declared an object size of 243,509,357 bytes and SHA-256 `446ead71292578830d99e690fd8dded73bc63d85df80f0cfba37762ccb330326`.

`git lfs pull` in that exact Repo project fetched an APK with the declared size and hash; `file` identified it as an Android package, `unzip -t` passed, and the Repo worktree remained clean. `scripts/setup.sh` now performs this pull explicitly so a clean checkout fails early if the object cannot be obtained.

The first incremental retry completed in 6 minutes 10 seconds. A second incremental invocation exercised the new post-build validation in `scripts/build.sh` and completed in 2 minutes 14 seconds. The latter is the current artifact set recorded below.

### Current artifacts

| File | Size | SHA-256 |
| --- | ---: | --- |
| `lineage-16.0-20261004-UNOFFICIAL-c5lte.zip` | 618,185,673 | `49d2bd7bc26f142716fc38791bc1bcab92f4369eb09349b561e80926ee0c2568` |
| `boot.img` | 11,116,560 | `92092a0650879662affee4e0b1f1180199d3e9fe41f8dae31a2b4be69928bcfe` |
| `recovery.img` | 16,236,560 | `0a4ceda490298d839d0200252c6acd84c84a8630179427262a2cb67a9eaea1d9` |
| target-files ZIP | 1,517,744,823 | `4af508f485e0c8f3d85797ac701fd011bd1c79abd8e9f666d556c2d90928ba63` |
| sparse `system.img` inside target-files staging | 1,482,363,172 | `d33b10c5ad4b32e799ed91af0e3b97723fa9e92c13fc3705dfe81e9a6c5b5f0f` |

The ROM and target-files ZIPs both pass a full `unzip -t`. `file` identifies the staged system image as Android sparse image v1.0 with 780,288 4-KiB output blocks in 22 chunks. The current recovery image again passes `scripts/inspect-recovery.sh` with zero failures and zero warnings.

### OTA structure and restrictions

- OTA type is `BLOCK`; the ROM contains `system.new.dat.br`, `system.transfer.list`, `boot.img`, the updater script, and metadata.
- Metadata declares `pre-device=c5lte`, SDK 28 and security patch level `2022-01-05`.
- The updater script asserts both device `c5lte` and bootloader `C5000ZHU1CRJ5`, writes the system block image, then writes `boot.img` to the boot partition. This exact bootloader restriction remains a stock/device verification item, not a proven compatibility guarantee.
- The package is an unsigned-development result in the release-management sense: it uses the AOSP/Lineage test key from the source tree. It is not a production release and has not been installed.

### Logs

| Invocation | Result | Log size | Log SHA-256 |
| --- | --- | ---: | --- |
| Initial full build | failed at unmaterialized WebView LFS pointer | 13,599,660 | `5825bc92657b90a01afa700bc3f3ad4c925f7f6ed65d2fe1e37e28293057277a` |
| Incremental retry | success | 526,957 | `7671b2da2d86e5f11f22ea347bd826a04a1daada32bbb06900ac29ce49a40caf` |
| Artifact-check validation run | success | 51,688 | `fe538a073ab6f676c4aed379d4d1a53d70b1fa394509c40e755a83004c748e5b` |

The first successful retry produced a 618,187,615-byte ZIP with SHA-256 `abe001eb0ef77343b1932ed6f1bda84e9e8bf2427d7719b02f6282e2157cfe08`. The validation run rebuilt the kernel and filesystem packaging and produced the different current hashes above. Successful repeated builds therefore establish dependency closure, but not byte-for-byte reproducibility. Kernel build identity/time and filesystem packaging inputs must be normalized before making a reproducible-build claim.

The build also emits nonfatal warnings about an absent `VENDOR/build.prop` and uses the legacy product layout; this does not substitute for verifying the public proprietary files against matching stock firmware.

No artifact has been flashed, booted, or tested on hardware.
