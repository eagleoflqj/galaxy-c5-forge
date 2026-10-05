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

### Packaging findings and resolution

1. `board/kernel.mk` declares ramdisk offset `0x02000000` and tags offset `0x01e00000`, but the original generated Ninja command passed only base and page size. A current boot image read from the CSA1 test device confirms the declared addresses (`0x82000000` and `0x81e00000`), while the bootable TWRP recovery uses mkbootimg defaults (`0x81000000` and `0x80000100`). The tracked c5lte patch now passes the device-declared offsets through `BOARD_MKBOOTIMG_ARGS`; both rebuilt images have the device-verified addresses, and `scripts/build.sh` rejects any future recovery or boot image that regresses to the defaults.
2. `hardware/samsung/mkbootimg.mk` sets the Loki command to `echo` when `TARGET_NEEDS_LOKI` is false, then attempts to copy the nonexistent `.lok` output under `|| true`. This produces a visible `cp: cannot stat` message but leaves the original image intact and does not fail the build. No evidence currently indicates that SM-C5000 should use Loki.
3. `unpack_bootimg` from the Ubuntu 26.04 host fails to decode this legacy image header, while `file`, `abootimg` and `binwalk` parse it. Use format-matched tooling and compare against an actual stock image rather than assuming the newest unpacker is authoritative for this image.

The original image was not flashed or tested. A successful compile did not resolve its header-offset discrepancy or prove that display, input, storage, USB, ADB, decryption, or reboot works on SM-C5000.

## Device-verified packaging rebuild — 2026-10-04

Status: **final candidate compiled, structurally verified and booted on the designated device under explicit authorization; display, touch, recovery UI, native root ADB, sideload transitions, BCB mapping and required partition discovery pass**.

The designated SM-C5000's current boot partition confirmed kernel `0x80008000`, ramdisk `0x82000000`, tags `0x81e00000`, page size 2,048 and a 33,554,432-byte recovery partition. The tracked source patch passes the already-declared c5lte ramdisk and tags offsets to mkbootimg and extends the bootloader allowlist from `C5000ZHU1CRJ5` to include the device's `C5000ZCU1CSA1`. `scripts/apply-patches.sh` applies this patch idempotently after source sync and before each build.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `recovery.img` | 16,236,560 | `1b88d313de380d4b0b427de1e6d292b06fb962292d2d4fbdb1d73d42f9938a1b` |
| `boot.img` | 11,116,560 | `779af490093517b78d6f659669aa31f5d3358b7d8e533e0c1766e3a99100bab7` |

Both images now use kernel `0x80008000`, ramdisk `0x82000000` and tags `0x81e00000`. The rebuilt recovery passes `scripts/inspect-recovery.sh` with zero failures and zero warnings, including the Samsung tail marker. The recovery rebuild log is `logs/build-recovery-20261004T021054Z.log`, 84,837 bytes, SHA-256 `59609e9521aa2a05db72407415167daf7c5eba38776167a6a08966dc0e30abac`.

### First recovery hardware test

With operation-specific authorization, the 16,236,560-byte candidate was written once to the confirmed 33,554,432-byte recovery target `/dev/block/mmcblk0p19` from the running TWRP environment. The preflight rechecked model `SM-C5000`, device `c5lte`, bootloader `C5000ZCU1CSA1`, target symlink, partition capacity, image size and SHA-256. Battery was 31% and charging. A same-length readback from the recovery partition produced the exact candidate SHA-256 before `adb reboot recovery` was issued.

The new recovery enumerated over USB after approximately 22 seconds as `18d1:d001`, product `SAMSUNG_Android`, with the expected device serial. The device screen displays the LineageOS logo, `RECOVERY`, the four expected main-menu buttons (`Reboot`, `Apply update`, `Factory reset`, `Advanced`), and build identity `Android 9` / `LineageOS 16.0-20261004-UNOFFICIAL-c5lte`; recovery boot and display therefore pass their first hardware test. Touch input opens `Advanced`, whose five expected entries are visible (`Reboot to bootloader`, `Reboot to recovery`, `Mount system`, `View logs`, `Power off`), so touch and menu navigation also pass. Both Android platform-tools 34.0.5 and the LineageOS 16 build's ADB 1.0.40 report the transport as `offline`. USB tracing shows the host sends `CNXN`, but the device endpoint returns no ADB packet and the bulk write times out; this is not an `unauthorized` key rejection. No partition other than recovery was written.

The on-device recovery log reports `Failed to mount /data: No such file or directory` and ten attempts to stat `/dev/block/bootdevice/by-name/fota`. Source and ramdisk inspection identified a concrete device-tree defect: the recovery hardware init was only a renamed copy of the power configuration and omitted the normal boot's `/dev/block/bootdevice` symlink creation. At this stage the missing parent link made every `by-name` path invalid, so the log did not establish that `fota` itself was wrong. A tracked fix now installs a dedicated recovery init that creates the bootdevice symlink and imports the original power configuration.

An initial interpretation incorrectly changed the `/misc` backing path from `fota` to `misc` while adding the init fix. The resulting second recovery candidate built successfully in 49 seconds. It is 16,236,560 bytes with SHA-256 `809b154187a4895dc85caa1f320c647b913af3b1c6c7a98e7e82f06a0540dcc5`; structural inspection reported zero failures and zero warnings. Its build log is `logs/build-recovery-20261004T024518Z.log`, 85,003 bytes, SHA-256 `eee50ce05d27069ddbfd054e80292e7eeb31ee601af64dc5d3882debfd17759c`. This intermediate candidate was not written to the phone and is superseded because the PIT has no partition named `MISC`.

The first recovery's dedicated `Apply from ADB` mode also enumerated as `18d1:d001` but remained `offline`, proving the failure was not limited to secure-key loading. Source comparison with the working TWRP and later official Lineage recovery branches exposed an independent USB ordering defect: Lineage 16 enabled the legacy gadget before forking minadbd, while the later upstream implementation explicitly forks minadbd first because USB readiness depends on the daemon opening FunctionFS. The same early-enable ordering could leave normal recovery ADB bound before its daemon was ready.

A second tracked patch backports the upstream USB state transitions to the pinned Lineage 16 recovery: normal ADB now transitions through `none`, starts root adbd, then enables `adb`; sideload starts minadbd before selecting the new `sideload` USB state; init handles `none` and `sideload` for legacy and configfs gadgets. The resulting third candidate built successfully in 25 seconds. It is 16,236,560 bytes with SHA-256 `39c4a31532eb2dcf9a54fdd94d4c899d2ba5335ba69b0ecfa73935a3d962d8b7`; structural inspection reports zero failures and zero warnings, all three ramdisk fixes are present, and its log is `logs/build-recovery-20261004T025524Z.log`, 10,008 bytes, SHA-256 `cd2a70059b519f469d92ab0a7eececb047e4c917eb19af48fb239d58eed06dd7`.

The working first recovery rebooted into Samsung Download Mode. Heimdall detected the device and read its PIT without flashing. PIT Entry #18 names `RECOVERY`, Identifier 19, offset 176128 and 65,536 512-byte sectors, confirming a 33,554,432-byte target and matching `/dev/block/mmcblk0p19`. The saved 8,192-byte PIT has SHA-256 `657d80ed96e111e857ff4fd9062fbb58aaf466e1248f36ce6fa8f61de609b1b0` and is kept mode 600 outside Git.

With a second operation-specific authorization, Heimdall 2.1.0 wrote only the PIT-confirmed `RECOVERY` target and reported `RECOVERY upload successful` at 100%; `--no-reboot` kept the device in Download Mode. The device was then booted directly to recovery with the hardware keys, and the third candidate again reached the Lineage recovery UI. Normal ADB still remains `offline`. The revised state transitions produce one expected gadget disconnect/re-enumeration during startup, after which USB remains enumerated as `18d1:d001`, but the device still does not answer the host's ADB connection. The corrected mount and service logs are being inspected through the recovery UI.

The third candidate can now mount cache and exposes `/cache/recovery/last_log`, proving that the recovery-specific bootdevice initialization works. Its log instead reports ten failed attempts to stat `/dev/block/bootdevice/by-name/misc`, followed by failures to set and clear the BCB. The downloaded PIT contains `BOTA` and `FOTA` but no `MISC`; more decisively, the ramdisk of the previously working TWRP maps `/misc` to `/dev/block/bootdevice/by-name/fota`, and the c5lte SELinux policy labels that same `fota` path as `misc_block_device`. This establishes that the earlier `misc` change was wrong: the first candidate's `fota` error was a consequence of the missing parent symlink, not a missing FOTA partition.

The tracked patch therefore retains the recovery-specific bootdevice initialization and restores the original, device-evidenced `fota` mapping. The resulting fourth candidate built successfully in 24 seconds. It is 16,236,560 bytes with SHA-256 `1ef193a4f9cb10f1fcffa4fc84c11a2731083942b875a819fe9a5797192a8d14`. `scripts/inspect-recovery.sh` reports zero failures and zero warnings; direct ramdisk inspection confirms the `fota` mapping, bootdevice symlink action, and the normal/sideload FunctionFS sequencing. Its build log is `logs/build-recovery-20261004T032221Z.log`, 8,752 bytes, SHA-256 `0d0fd1605d4ebd43753e27c830522761747d9490afc64ef7fa3338f44c875c5d`.

With a third operation-specific authorization and the battery reported at 55%, a final preflight rechecked Samsung Download Mode USB `04e8:685d`, Heimdall detection, the exact image hash and size, the saved PIT hash, and Entry #18 as writable `RECOVERY`, Identifier 19, offset 176128, count 65536. The first preflight invocation stopped before flashing because its parser expected the label `Partition Identifier` while Heimdall printed `Identifier`; no upload command ran in that invocation. After correcting only that read-only assertion, the same checks passed and Heimdall 2.1.0 wrote only `RECOVERY` with `--no-reboot`. It reported `RECOVERY upload successful` at 100%. No other partition was selected or written.

### ADB isolation diagnostics

The fourth recovery eliminated the BCB and partition-path errors but left both normal and sideload ADB offline. Four separately authorized recovery-only diagnostics then changed one variable group at a time:

| Candidate | Change from the fourth source build | Size | SHA-256 | Result |
| --- | --- | ---: | --- | --- |
| Fifth | force permissive SELinux from boot | 16,236,560 | `fe4a5ffeb8f3d1377667176047a6e23ba2f067d0de54b9e69ac062d189701c68` | ADB remained offline |
| Sixth | TWRP 3.10.84 kernel and DT with the Lineage ramdisk | 17,575,952 | `58c7d1e6dd34b204c589a1ac11f6e7a7db891c171b4816e34af369778dddf2f1` | ADB remained offline |
| Seventh | sixth candidate plus only TWRP's Android 27 `/sbin/adbd` | 17,563,664 | `84f0aa844f4d5b9f0b0be07150e56440e0b9cca3b81c8428f9ada94e72c9bf1d` | transport immediately became `recovery`; shell dropped privileges because the old daemon did not understand the Lineage root property |
| Eighth | seventh candidate plus compatible root-property init actions | 17,563,664 | `fb6527da736bddb42895b0590e20fc344ccabc28c834822c9163772c3865d897` | stable root recovery ADB; enabled complete logs and live hardware inspection |

The sixth and seventh candidates differ only in `/sbin/adbd`, so their opposite transport results isolate the failure to the Android 28 daemon rather than kernel, DT, SELinux or gadget enumeration. The Lineage Android 28 daemon has SHA-256 `2400978e081d9e126d47c757b5a4890c983e75130222c794d8a941372fa96166`; the working TWRP Android 27 daemon has SHA-256 `653c0270b9e9e164ecdd22ccba4ec48b231546907b889dcc50401553cc7d8f7b`.

Source inspection found that Android 28 `adbd` defaults to FunctionFS AIO and explicitly selects synchronous I/O when `sys.usb.ffs.aio_compat=1`, with a comment that kernels older than 3.18 need this fallback unless AIO support was backported. c5lte's normal system USB init already sets the property, but recovery does not import that init file. The designated device showed the property unset in recovery. The tracked device patch now sets it during recovery `early-init`, before adbd starts. The recovery patch also removes a dead init action that attempted to start an undefined `setup_adbd` service; the recovery program itself performs the key check and starts ADB.

### Final recovery build and hardware validation

The final incremental build completed successfully in 23 seconds. Its log is `logs/build-recovery-20261004T044731Z.log`, 8,752 bytes, SHA-256 `a8fafc883eb61562e207a3aa5fae15bc00fd0f4f8487285ad1e3a3792104f0b4`. The resulting image is 16,234,512 bytes with SHA-256 `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`. Automated inspection reports zero failures and zero warnings; the image uses kernel `0x80008000`, ramdisk `0x82000000`, tags `0x81e00000`, contains the source-built Android 28 adbd with the expected hash, includes `sys.usb.ffs.aio_compat=1`, and has no diagnostic TWRP binary or kernel substitution.

With explicit operation-specific authorization, preflight reconfirmed model `SM-C5000`, device `c5lte`, bootloader `C5000ZCU1CSA1`, battery 63% and charging, exact image hash/size, Samsung Download Mode USB `04e8:685d`, and live PIT Entry #18 as read/write `RECOVERY`, Identifier 19, offset 176128 and count 65536. Heimdall 2.1.0 wrote only `RECOVERY` with `--no-reboot` and reported a successful 100% upload. The eighth diagnostic image and complete TWRP partition backup remain available outside Git as rollback images.

The final recovery booted with the source-built `3.10.108-g71b335bed49` kernel. ADB immediately reported `recovery`; `adb shell` ran as uid 0 in `u:r:su:s0`; `sys.usb.ffs.aio_compat`, `sys.usb.ffs.ready`, `sys.usb.config` and `sys.usb.state` were respectively `1`, `1`, `adb` and `adb`; the legacy gadget reported `CONFIGURED`; and the live `/sbin/adbd` hash matched the build. The recovery source intentionally switches non-release builds to SELinux permissive, so `getenforce=Permissive` is expected and is not inherited from the eighth diagnostic image.

The required by-name links resolved correctly: boot p18, recovery p19, FOTA p21, system p36, cache p37 and userdata p38. The recovery log contains `/misc` backed by `fota` and no BCB, FOTA, data, cache, AIO or FunctionFS failure. The only persistent display warning is the absence of a DRM device; framebuffer display and touch are working, so it is nonblocking. Entering `Apply from ADB` changed the host transport to stable `sideload` over USB `18d1:d001` without sending a package. Cancelling sideload automatically restored stable root `recovery` ADB with the same compatibility property and configured gadget. Mode-600 logs and state snapshots are retained outside Git under the device evidence directory.

A later read-only pre-install preflight hashed exactly the first 16,234,512 bytes of the live recovery partition and reproduced `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`, confirming that the installed recovery still exactly matches the hardware-tested ninth image. The live boot partition still reproduced the original backup SHA-256 `1ab3c211b4c03bc88b92d685ff631e265064f78155360706faa131f9706acc29` before any ROM installation.

## First full ROM build — 2026-10-03/04

Status: **compiled successfully after one diagnosed source-materialization failure; installed and booted on the designated hardware, with bring-up still incomplete**.

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

### Installed-baseline build artifacts

| File | Size | SHA-256 |
| --- | ---: | --- |
| `lineage-16.0-20261004-UNOFFICIAL-c5lte.zip` | 618,085,215 | `f6d6d9e0c6bcf9762a61662ae68613457391fa6ef16d4f974a568bd954e4b52c` |
| `boot.img` | 11,114,512 | `2e9274e34014d4c4a6c3c2a5cde55ad4f22dd0e48b357ebc28ae899e567b9bbb` |
| `recovery.img` | 16,234,512 | `919e4cc976d6a1531c531d0e53d0eddae5206845d3da64234bc23d7b3e080058` |
| target-files ZIP | 1,517,758,478 | `1bea4e28aa109985bc4c54beda77ee7cb2495e3920562bcd52e3d43619bd49b0` |
| sparse `system.img` inside target-files staging | 1,482,367,268 | `930ad89c1c05b8c15f1fc3c01adcd22d64f4512239b16bdeb4e2825547ccdb5b` |

The ROM and target-files ZIPs both pass a full `unzip -t`. `file` identifies the staged system image as Android sparse image v1.0 with 780,288 4-KiB output blocks in 22 chunks. The current recovery image contains the same final source fixes and native Android 28 adbd as the hardware-tested ninth image. Its byte hash differs because the ROM rebuild regenerated the kernel with a later build timestamp; the ROM updater does not directly write recovery.

### OTA structure and restrictions

- OTA type is `BLOCK`; the ROM contains `system.new.dat.br`, `system.transfer.list`, `boot.img`, the updater script, and metadata.
- Metadata declares `pre-device=c5lte`, SDK 28 and security patch level `2022-01-05`.
- The updater script asserts device `c5lte` and permits bootloader `C5000ZHU1CRJ5` or the designated test device's `C5000ZCU1CSA1`, then writes the system block image and `boot.img`. Adding CSA1 is based on the exact test-device evidence, but it does not by itself prove that the mixed BRA1/CRJ5 public proprietary baseline is compatible.
- The package is an unsigned-development result in the release-management sense: it uses the AOSP/Lineage test key from the source tree. It is not a production release; this exact baseline was later installed under explicit authorization.

### Logs

| Invocation | Result | Log size | Log SHA-256 |
| --- | --- | ---: | --- |
| Initial full build | failed at unmaterialized WebView LFS pointer | 13,599,660 | `5825bc92657b90a01afa700bc3f3ad4c925f7f6ed65d2fe1e37e28293057277a` |
| Incremental retry | success | 526,957 | `7671b2da2d86e5f11f22ea347bd826a04a1daada32bbb06900ac29ce49a40caf` |
| Artifact-check validation run | success | 51,688 | `fe538a073ab6f676c4aed379d4d1a53d70b1fa394509c40e755a83004c748e5b` |
| Device-verified recovery rebuild | success | 84,837 | `59609e9521aa2a05db72407415167daf7c5eba38776167a6a08966dc0e30abac` |
| Device-verified ROM rebuild | success | 51,906 | `9ca6495b60dd4f2afed3758bdb721e79ab2aef946d3579b4d10401c297b3fb39` |
| Final recovery patch-set ROM rebuild | success | 52,017 | `ec065d08c39e3c1fa5b5995d5af71a62a3fa50910f74306ac20350f24f70cbe1` |

The first successful retry produced a 618,187,615-byte ZIP with SHA-256 `abe001eb0ef77343b1932ed6f1bda84e9e8bf2427d7719b02f6282e2157cfe08`. The validation run rebuilt the kernel and filesystem packaging and produced the different current hashes above. Successful repeated builds therefore establish dependency closure, but not byte-for-byte reproducibility. Kernel build identity/time and filesystem packaging inputs must be normalized before making a reproducible-build claim.

The build also emits nonfatal warnings about an absent `VENDOR/build.prop` and uses the legacy product layout; this does not substitute for verifying the public proprietary files against matching stock firmware.

Before the first installation, static inspection of the final updater script confirmed that it asserts device `c5lte` and bootloader `C5000ZCU1CRJ5` or `C5000ZCU1CSA1`, updates only `/dev/block/bootdevice/by-name/system`, and extracts `boot.img` only to `/dev/block/bootdevice/by-name/boot`. It contains no direct recovery, PIT, modem or EFS write. The installed system carries `install-recovery.sh`, but its init service is disabled unless `persist.sys.recovery_update=true`; that property is not preset by this build.

### First ROM installation and boot

With explicit authorization and battery at 100%, the verified Lineage recovery performed `Wipe data / factory reset`. Although the UI returned quickly to the menu, `/tmp/recovery.log` explicitly records `Formatting /data...`, `Formatting /cache...` and `Data wipe complete.` The exact final ROM ZIP was then sent through the verified sideload transport. Host ADB reported `Total xfer: 1.00x`; recovery verified the package signature, wrote the expected 365,441 system blocks, deleted its empty stash, recorded `last_install` status `1` and printed `Install complete.` The nonfatal `failed to read uncrypt status` warning occurred after the successful updater result and refers to an absent optional status file.

Before reboot, a read-only prefix hash of the live boot partition matched the built 11,114,512-byte boot image SHA-256 `2e9274e34014d4c4a6c3c2a5cde55ad4f22dd0e48b357ebc28ae899e567b9bbb`. A read-only `ro,noload` system mount confirmed Android 9, model `SM-C5000`, device `c5lte`, Lineage version `16.0-20261004-UNOFFICIAL-c5lte`, security patch `2022-01-05` and the CSA1-inclusive bootloader assertion. The recovery prefix still matched the exact hardware-tested ninth image SHA-256 `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`.

The device then rebooted to system and reached the LineageOS setup wizard. The user reported the centered Lineage logo/text and working `EMERGENCY CALL` and `NEXT` controls. This verifies initial boot, display and setup UI; it does not yet validate telephony or the remaining hardware matrix. Installation logs and state snapshots are retained outside Git with mode 600.

## Wi-Fi firmware-link fix build — 2026-10-04

Root-enabled first-boot diagnostics isolated the Wi-Fi failure to one missing generated system link. The modem firmware partition contains `adsp.b15`, 131,444 bytes, SHA-256 `a67fd891958e80bdca7d2a835f6d09c500c0f1b7c8d52c0e55b6dc6bfe87d0fa`, and `adsp.mdt` declares that exact segment size. The device `Android.mk` linked `adsp.b00` through `adsp.b14` but omitted `b15`, so the kernel firmware loader could not complete ADSP startup. The tracked `0002-link-complete-adsp-firmware-set.patch` adds only that missing list entry.

The incremental `bacon` build completed successfully in 2 minutes 28 seconds. The build log is `logs/build-rom-20261004T125810Z.log`, 127,543 bytes, SHA-256 `ba982d73e67cfb6376e1970d61d38a8571ad7847f64043ac9e8c616299537827`.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `lineage-16.0-20261004-UNOFFICIAL-c5lte.zip` | 618,153,042 | `099ba78ec9ebe8320e613087901f9d1cf46d2137ee8be6be1abdf8300b6c6359` |
| `boot.img` | 11,114,512 | `369a0444bdfdc37dc6336ec2f80cf2f54bdc707a3278f3dfe009dae9c6ef5902` |
| tracked fix patch | 495 | `81c6e8e39a53b3a27da024b0396352797b447bd6980c39f854883ea2a47b5f5c` |

The output tree and target-files ZIP both contain `SYSTEM/etc/firmware/adsp.b15` as a 30-byte symbolic link to `/firmware-modem/image/adsp.b15`. Full ZIP integrity, required entries, `pre-device=c5lte`, the CSA1 bootloader assertion and device-verified boot addresses pass. Static inspection again shows only the system block update and boot extraction; there are no userdata, cache, recovery, firmware, modem, EFS or PIT writes. At this build stage the candidate had not yet been installed or hardware-validated.

Before an update was proposed, the live installed boot and system partitions were captured read-only over root ADB to a mode-600 directory outside Git. Their complete uncompressed lengths/hashes are 33,554,432 bytes / `59999d69d6270e4aa5d934fedc620e9fb0d1e054892d1f2041998fa5ed978be0` for boot and 3,196,059,648 bytes / `eee2e8352a0170e6c99e43e52df896db0c6ec0edeba424793da442446978b886` for system. Both compressed images pass `zstd -t`, providing an exact pre-update rollback source without reading userdata.

### Wi-Fi-fix installation and first hardware result

Under exact operation-specific authorization, recovery sideload completed with `Total xfer: 1.00x`. Recovery wrote all 365,442 expected system blocks, recorded result `1` and `Install complete`, and showed no updater failure. A read-only post-install check matched the expected boot prefix SHA-256 `369a0444bdfdc37dc6336ec2f80cf2f54bdc707a3278f3dfe009dae9c6ef5902`, preserved the installed recovery prefix SHA-256 `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`, and resolved the new system link to `/firmware-modem/image/adsp.b15`. Userdata and cache were preserved.

After reboot, early dmesg shows `adsp.b15` loading and ADSP completing reset at `0x8c600000`–`0x8de00000`; WCNSS then loads at `0x8de00000`–`0x8e41d000`, completes reset, reports firmware version/build and downloads its NV data. Neither the former missing-`adsp.b15` messages nor WCNSS memory-setup failure occurs. Android reaches boot completion under SELinux enforcing with `wlan.driver.status=ok`, all Wi-Fi userspace services running, the saved Wi-Fi setting on, and `wlan0`/`p2p0` present. This verifies the firmware and driver bring-up fix; access-point discovery, association, DHCP, traffic and reconnect remain untested.

Mode-600 external evidence includes the recovery log (SHA-256 `06ad493c85981e58b6b42c2ec7d66007ae86a0aebeaced510c017afefcfede61`), host sideload transcript (`bfb03f93311e20fa157a9d4f970e2492a62046a6ae9089226848bd1af1ec5843`), early post-fix dmesg (`42e5e8c9a8d16b97c700e675b3222b1a203071b34b7be761f5263a9e5343be45`), early logcat (`4f4a6320edfe72e8998290a74da773d2f0f8bfb5adde6aaf918144406b173a69`) and a sanitized pre-association state file (`d673037adfa5e6793972e9d2b010cb9b8aeaf972e25a579d79add31ba4fd252b`).

The user then confirmed network discovery and completed association in the Android UI. Sanitized ADB checks recorded only booleans/status: carrier up, global IPv4 and default route present, direct IP and DNS-name traffic passing, all Wi-Fi services running and SELinux enforcing. A normal reboot restored ADB in about 26 seconds and Wi-Fi automatically re-associated with IPv4 in another 24 seconds; the same route/traffic checks passed again, and the second-boot dmesg still contained successful ADSP/WCNSS startup with none of the former failure signatures. The sanitized association record has SHA-256 `4b69ad739d02560cc6d0092b583274a4169d440ccc739260f7913ce7db9fff89`; the post-reboot dmesg and sanitized reconnect record have SHA-256 values `bd5264f4d0cbe956b24cec1bb9a9e818bdf76c8514555e8ca0519ea66ab17247` and `6dac8669f4424ac09866ce597ab7e641d7393026331144ac1a4526c5812405b0`. No SSID, BSSID, password or local address was retained.

## CSA1 GNSS repair iterations — 2026-10-04

The user supplied `SAMFW.COM_SM-C5000_CHC_C5000ZCU1CSA1_fac.zip`, a matching CHC factory archive. The original 2,247,133,405-byte ZIP has MD5 `887ed433bb432840ba3a9b341b215295` and SHA-256 `36946cd31d1817fb3a6d9e7b5fb4f2072df3dce5169eb644d86446801a860dc9`; `unzip -t` passes. Its AP member is `AP_C5000ZCU1CSA1_CL14615209_QB21439331_REV00_user_low_ship_MULTI_CERT_meta.tar.md5`. The extracted 3,196,059,648-byte raw system image has SHA-256 `1a113c05150d765a5e8142eac2b3d4bb29b2eecbd776fd37092a6338678f50b6`. These files are retained outside Git under `/mnt/storage/android/galaxy-c5-stock/C5000ZCU1CSA1-CHC` with restrictive permissions.

The stock-system evidence set contains 75 files, including dynamic dependencies and isolated stock platform libraries used only for ABI comparison. Its regenerated mode-600 manifests have these hashes:

| Manifest | SHA-256 |
| --- | --- |
| `SHA256SUMS` | `94497ac7380d5ab27e08c8be052642fa2914ded1974083d7472961dc9e1c7a58` |
| `FILE-TYPES.txt` | `626c29966344440655db1e18463ceb556b9cb1927632177d1fd70cc248b748ef` |
| `ELF-NEEDED.txt` | `aee80bac4f54639896d7a19b195323c3dcefc9702f3dfc9f1270405c7b99c255` |

`scripts/import-stock-gnss.sh` accepts only that exact raw-system size and SHA-256, applies the tracked source patches idempotently, extracts the 45 files required by the candidate, checks an embedded stock SHA-256 for every file, performs the reviewed ELF dependency rewrites, verifies 26 post-rewrite hashes and invokes the device tree's vendor-file generator. The extracted proprietary bytes remain only in the external Android worktree and are not part of this Apache-2.0 repository.

The stock service and implementations are 32-bit ARM Android 8 binaries. Static comparison found that stock `vendor.qti.gnss@1.0_vendor.so` requires three `android.hardware.gnss@1.0` C++ `toString` functions that Android 9 inlines and no longer exports. The tracked patch builds `libshims_gnss_hidl.so` with the exact three mangled symbols and attaches it only to that stock library through `TARGET_LD_SHIM_LIBS`.

The first GNSS candidate attempted to avoid globally replacing common QTI libraries by retaining stock SONAMEs under `_csa1` filenames and supplying them only through `LD_PRELOAD`. Recovery installed that candidate successfully, but the Android boot test proved the assumption wrong: `loc_launcher` could not resolve `libgps.utils.so`, the GNSS implementation could not resolve `libloc_core.so`, both services restarted every five seconds and `system_server` waited indefinitely for `IGnss/default`. The device therefore remained in the boot animation. Installation success did not imply a bootable userspace.

The replacement keeps the isolation goal without `LD_PRELOAD`. `scripts/patch-elf-dynstr.py` accepts only little-endian ELF files with one valid `.dynstr`, requires exactly one old string entry for each requested replacement, rejects longer replacement names and changes no section size. The importer rewrites every stock reference and provider SONAME from `libsecnativefeature.so`, `libgps.utils.so` and `libloc_core.so` to the explicit unique names `libsecgnssfeature.so`, `libgps.csa1.so` and `libloc_c5.so`. It requires rewrite counts of 7, 25 and 19 files respectively and verifies exact post-rewrite SHA-256 values for all 26 changed ELFs.

The patch also replaces the generic Android GNSS service and `gps.msm8952` implementation, restores the Samsung service/init definition, imports the stock GNSS configuration and QTI location components, declares both standard `IGnss/default` and QTI `ILocHidlGnss/gnss_vendor` VINTF instances, creates the stock data paths, and adds the required service/file contexts and narrow SELinux access. A first incremental build exposed stale generic GNSS files left in the product staging tree after their package removal. `scripts/build.sh` supports `--installclean`, rejects the generic service/HAL and stale `_csa1` libraries, verifies all 45 final output hashes, rejects old DT_NEEDED/SONAME strings, requires the same 7/25/19 rewrite counts and rejects any remaining GNSS-service `LD_PRELOAD`.

The first candidate build completed in 2 minutes 10 seconds. Its log is `logs/build-rom-20261004T200442Z.log`, SHA-256 `338d974b1f43d41eabe2b51b76188787980353738f55d60d46c363ecd5c1aee8`.

Exact mode-600 copies of the first ZIP, boot image and build log are preserved outside Git under `/mnt/storage/android/galaxy-c5-build-artifacts/gnss-csa1-d3413774`, so a later build cannot silently replace the reviewed bytes.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `lineage-16.0-20261004-UNOFFICIAL-c5lte.zip` | 620,082,686 | `d3413774dc970ddf2e813dd0761abe3b54d7526785bbadc8be03f2abeae79b4d` |
| `boot.img` | 11,114,512 | `69d0f8b161a0e4d54aa059df0b98b5452ff53d4467bf75c2fbcd8bca54318bb6` |

Full ZIP CRC and metadata checks passed. The updater asserts `c5lte` and bootloader `C5000ZHU1CRJ5` or `C5000ZCU1CSA1`, contains only the system block payload and `boot.img`, updates `/dev/block/bootdevice/by-name/system` and writes `/dev/block/bootdevice/by-name/boot`; it has no recovery, userdata, cache, modem, firmware, EFS, PIT or repartitioning payload. The system image uses 376,136 of 780,288 4-KiB blocks. Final target-files metadata assigns mode `0755` and the expected SELinux domains to the Samsung service and `loc_launcher`, with libraries at `0644`.

Offline verification found all 45 imported output hashes exact, all required GNSS VINTF/init/context definitions present, all three compatibility symbols exported by the shim and every normalized strong undefined symbol from the 45 imported ELFs present in the final 32-bit system/vendor provider union. The first candidate was subsequently installed under exact authorization and failed the linker-namespace boot test described above.

The replacement clean build completed in 2 minutes 38 seconds. Its mode-600 evidence is preserved under `/mnt/storage/android/galaxy-c5-build-artifacts/gnss-csa1-d7002bdb`.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `lineage-16.0-20261004-UNOFFICIAL-c5lte.zip` | 620,083,081 | `d7002bdb040094d1892927a0d8d00502dd049a4ca5a3e5adee2fde13e9aadecd` |
| `boot.img` | 11,114,512 | `b97af8d517902851aa7d074c725f2f2c3389d793b5f35a41e9b085a500f2cd0a` |
| `build-rom-20261004T204954Z.log` | 602,195 | `d19a84c07fa7c55d5e0d4b7e50d23263f91491004988cac43583c12038f56891` |

The replacement ZIP passes CRC, c5lte metadata, boot-address and updater-scope checks. Its system image again uses 376,136 of 780,288 4-KiB blocks. All 45 final file hashes, all 26 rewritten-ELF hashes, rewrite counts, explicit new dependencies, absence of old dependency names, absence of the generic GNSS artifacts and absence of GNSS `LD_PRELOAD` pass.

The replacement was subsequently installed under exact authorization. Recovery wrote all 366,745 expected system blocks, recorded updater status `1` and `Install complete`, and preserved userdata/cache. The boot prefix, recovery prefix and all 45 installed GNSS file hashes matched. Android completed boot in about 37 seconds; the Samsung GNSS service and `loc_launcher` remained at stable PIDs under SELinux enforcing and registered `IGnss/default`, `IGnss/gnss_vendor` and `ILocHidlGnss/gnss_vendor`. No old missing-library, dlopen or fatal-process error recurred.

Startup exposed one remaining compatibility difference. The current 32-bit `libperipheral_client.so` from the public baseline uses ordinary `/dev/binder`, causing the GNSS domain to be denied calls to framework `servicemanager` and failing its GPS registration with `vendor.qcom.PeripheralManager`. The matching CSA1 stock 32-bit library instead calls `ProcessState::initWithDriver` for `/dev/vndbinder`, where the already-running `pm-service` registers; the only built 32-bit consumer is the newly imported `liblbs_core.so`.

Functional location testing proves that this mismatch is non-blocking in the tested scope. SatStat detected 17 satellites, Organic Maps identified the correct location and compass direction worked. A sanitized read-only snapshot recorded 162 location reports, three TTFF reports with a mean of 5.67 seconds, a mean top-four satellite C/N0 of 30.8661 dB-Hz, stable GNSS service processes, SELinux enforcing and no missing-library, dlopen or fatal-process errors. No coordinates or personal identifiers were retained. The external mode-600 functional record has SHA-256 `9e791720680b26277044f616e7420bdb0300f237c49a4eac2f65bbcd72902a90`. This successful result does not justify importing another stock library or flashing a third GNSS candidate; repeat cold-fix and longer-duration testing can proceed on the installed build.

## CSA1 sensor-stack repair candidate — 2026-10-04

Paseo remained at zero after sustained walking despite successfully registering the framework's Samsung Step Counter with no permission requirement on API 28. Sanitized kernel/HAL logs show handle 19 and BHY custom sensor 60 enabling normally, changing BHA250 acceleration values during movement, repeated invalid FIFO records for sensor types 59/60, zero step differences and sequential step-processing errors. This proves that the failure is below the application and framework registration layers.

The public vendor tree supplied a 28,388-byte `ram_patch.fw` with SHA-256 `6af5eefca2de06838e9d5566dc58d1cf667a8017b5acec21fa95def368671468` and a 100,048-byte Android 7 `sensors.c5lte.so` with SHA-256 `1cbcbb1cdeee6a8c65c7866a82b97049052e5e8cbbf51d4776ddc82106859670`. The exact CSA1 raw system instead contains a 28,476-byte sensor-hub firmware with SHA-256 `ce23cc0006731977353a9d42168c5949783ff6a2882807a42969a65183136be4` and a 120,508-byte Android 8 legacy HAL with SHA-256 `96d03f6dc03202a510d2a75c7a39f72d58d871cbc5c8dfe104aaf818209a160a`. The invalid custom-pedometer records made the firmware mismatch the leading cause, while the HAL was retained as part of the matched stock pair because the then-installed proximity path also lacked a successful transition.

`scripts/import-stock-sensors.sh` accepts only the exact 3,196,059,648-byte stock image with SHA-256 `1a113c05150d765a5e8142eac2b3d4bb29b2eecbd776fd37092a6338678f50b6`, extracts only those two files, verifies their hashes, sizes, ELF architecture, seven-library dependency set and legacy `HMI` export, then regenerates the external vendor makefiles. The tracked device patch SHA-pins both source entries. The stock HAL's 86 undefined symbols all resolve from its declared Android 9 32-bit providers, and it introduces no proprietary dependency. Repeated imports are idempotent and proprietary bytes remain outside Git.

The `installclean` ROM build completed in 2 minutes 41 seconds. Mode-600 reviewed copies are preserved outside Git under `/mnt/storage/android/galaxy-c5-build-artifacts/sensors-csa1-71e3f1a9`.

| File | Size | SHA-256 |
| --- | ---: | --- |
| `lineage-16.0-20261004-UNOFFICIAL-c5lte.zip` | 620,087,151 | `71e3f1a9b3b92a85e6c2bf8dcdd277aa43ff015cc5ccb7732d81208fdc8a4029` |
| `boot.img` | 11,114,512 | `90f917a9e34be163592190093b5b43bbf05ed005c3263882455351c67387cc66` |
| `build-rom-20261004T231926Z.log` | 602,064 | `a9b6c96c88a9657f16415f80cb21e7cd36f5794a1b2c4e8a6374849d8c7bb44e` |

ZIP CRC, metadata, c5lte/CSA1 assertions, boot addresses, all prior GNSS checks, both sensor hashes, target-files hashes, mode `0644`, existing SELinux labels and sensor HAL symbol closure pass. The system image uses 366,750 output blocks. Static updater inspection shows only a block update of `/dev/block/bootdevice/by-name/system` and extraction of `boot.img` to `/dev/block/bootdevice/by-name/boot`; the ZIP contains no userdata, cache, recovery, modem, EFS, PIT or repartitioning payload.

The exact ZIP was subsequently installed under explicit authorization. The host returned `Total xfer: 1.00x`; recovery verified the package, wrote all 366,750 expected system blocks and reported `Install complete` with no updater error or userdata/cache formatting record. Before reboot, the installed boot prefix matched `90f917a9e34be163592190093b5b43bbf05ed005c3263882455351c67387cc66`, recovery remained `8b6d6457fa4e0a4fff51b4e3b218da253c8175be41fda89e52eebfa9426f3a6f`, the firmware matched `ce23cc0006731977353a9d42168c5949783ff6a2882807a42969a65183136be4` and the HAL matched `96d03f6dc03202a510d2a75c7a39f72d58d871cbc5c8dfe104aaf818209a160a`. Android completed boot in about 38 seconds under SELinux enforcing. Paseo then counted successfully; SensorService retained a 0-to-48 event sequence and BHY logged matching positive step differences without the former invalid-FIFO or sequential-processing errors. Wi-Fi remained `ok`, the GNSS process remained present and filtered logs contained no sensor/GNSS linker or fatal-process error. The recovery log observed before reboot had SHA-256 `6009590bfbf2759bca09c89f20e17b46b843c03c96de8a32829297bff8da95c3`.
