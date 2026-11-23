# Android system provenance

For the ordinary Android baseline, select 17.1 explicitly when running the preparation and ROM build commands. The prepared checkout must contain the device-specific proprietary inputs. The local manifest pins the device projects and setup applies the 790 revisions in the 17.1 locked manifest before sync. The resolved checkout manifest is saved alongside the sync log.

```sh
./scripts/setup.sh --workdir "$ANDROID_WORK" --lineage-version 17.1
LINEAGE_VERSION=17.1 ./scripts/lineage-env.sh build-image
./scripts/build.sh --workdir "$ANDROID_WORK" --lineage-version 17.1 --target rom
```

The ordinary ROM build produces a recovery-installable ZIP named `lineage-17.1-YYYYMMDD-UNOFFICIAL-c5lte.zip`. The tested Linux 4.9 build uses the BOOT and SYSTEM partition images listed in the README.

The accepted Android SYSTEM uses the installed LineageOS 17.1 build as its base. Its 5,116 filesystem entries can be compared with the 5,115 entries in the original October 8 target-files package. Thirty regular files changed bytes, one startup file was added, and the addon survival script acquired execute permissions. All other paths matched for contents, file type, permissions, owners, symlink targets and extended attributes.

The [changed-file manifest](../manifests/system-changes.tsv) identifies the installed payloads by path and SHA-256. The Settings, SystemUI and Recorder packages each include the APK and its matching arm64 odex and vdex. A rebuilt APK with stale precompiled files does not match the accepted combination. Other changes cover boot-time USB selection, storage mounting, audio and lights, the vendor daemons and the associated device policy.

The last two SYSTEM updates used the same base image. Settings changed 12,656 blocks. The USB init and configfs changes touched 191 other blocks. Replaying those changes reconstructed the accepted full-partition SHA-256. This step takes already built components as input. The complete accepted SYSTEM still requires separately built components.

To find a source patch, look under `patches/lineage-<version>/` or `patches/shared/` and follow the project path from the Android checkout, such as `system/core/`. The apply script selects the shared patches and any file filters for each version. The 17.1 USB init generator reads its ordered series from `patches/lineage-17.1/usb-init/`. Optional diagnostic patches live in `patches/diagnostics/`.

The device policy series targets the pinned device checkout. Regenerate the vendor makefiles from the patched proprietary-files.txt using the device's setup-makefiles.sh. A clean replay reproduced Android.bp, Android.mk, BoardConfigVendor.mk and c5lte-vendor.mk from those inputs.

From a prepared Android checkout, run `./scripts/build-usb-init.sh "$ANDROID_WORK" "$OUTPUT_PARENT"` to generate the four accepted USB init files. The command prints the path of its new output directory. All four outputs are checked against the accepted image.

To package the verified full SYSTEM image under its release filename, run the following command from the output directory. The README lists the SHA-256 for the compressed file and the raw partition image.

```sh
gzip -n -6 -c "$VERIFIED_RAW_SYSTEM" > lineage-17.1-20261121-c5lte-system.img.gz
```

## Independent baseline build

A clean, offline `systemimage` build from the prepared 17.1 checkout completed on Linux. A file-by-file comparison with the archived October target-files SYSTEM found 5,101 identical entries out of 5,115. The comparison covered regular-file contents, type, mode, owner, group, symlink target and extended attributes.

The standalone system image omitted the two recovery-install files present in target-files. Twelve shared paths differed in content. Four of those files matched the accepted payloads exactly: Settings.apk, its arm64 odex and vdex, and vendor/etc/sec_config. The other eight were two build-property files, the license notice, the boot animation, Messaging, and Snap with its odex and vdex. All 480 boot-animation frames differed only in their PNG timestamp chunk. The fresh baseline image has a different full-image digest from the archived baseline. Use the accepted image hashes in the README when identifying a tested image.

## LineageOS 18.1 with the 4.9 kernel

The Android 11 build uses `lineage_c5lte-userdebug`, the msm8952 CAF projects and the kernel and Rev09 QCDT described in the kernel build notes. The build root contains the Android checkout, private inputs, compiler output and logs. Keep the complete root on a persistent build disk. The platform manifest tracks 865 projects. The private vendor tree and the prepared kernel source are additional inputs.

Set `ROOT` to the absolute path of the private build root. For a fresh checkout, prepare the platform source from the forge repository with the following command.

```sh
./scripts/setup.sh --lineage-version 18.1 --workdir "$ROOT/android"
```

The private input archive contains all three kernel Git object databases, the frozen compilers, the controller firmware, the host environment and the prepared CSA1 vendor tree. With a fresh Android checkout, verify the archived files and reconstruct the kernel using these commands from the forge repository. Both the kernel source and output arguments must name new directories.

```sh
(
    set -eu
    inputs="$ROOT/inputs"
    source_tree="$ROOT/android/kernel/samsung/c5lte"
    mkdir -p "$(dirname "$source_tree")" "$ROOT/tmp"
    (cd "$inputs" && sha256sum --quiet -c migrated-files.sha256)
    TMPDIR="$ROOT/tmp" ./scripts/prepare-kernel.sh "$inputs/kernel-git/qcom.git" "$inputs/kernel-git/samsung.git" "$inputs/kernel-git/stable.git" "$source_tree"
    ./scripts/build-kernel.sh "$source_tree" "$inputs/toolchains/clang" "$inputs/toolchains/arm64" "$inputs/toolchains/arm32" "$inputs/firmware" "$ROOT/kernel-build" 8
)
```

The prepared CSA1 tree at `$ROOT/inputs/vendor-c5lte` contains the vendor makefiles, libraries, firmware and services used by the 17.1 baseline. Populate a fresh `vendor/samsung/c5lte` from that input. The following commands apply the three binary transformations for Linux 4.9 and pair the radio settings library with the protobuf ABI from the pinned VNDK 29 snapshot. The radio uses a distinct library name so Android 11 can continue using its platform protobuf library.

```sh
(
    set -eu
    base="$ROOT/inputs/vendor-c5lte"
    vendor="$ROOT/android/vendor/samsung/c5lte"
    mkdir -p "$(dirname "$vendor")"
    mkdir "$vendor"
    cp -a "$base/." "$vendor/"
    temporary=$(mktemp -d "$ROOT/tmp/vendor.XXXXXX")
    for pair in qmuxd-poll:bin/qmuxd thermal-engine-units:bin/thermal-engine time-genoff:lib64/libtime_genoff.so; do
        payload="proprietary/vendor/${pair#*:}"
        python3 "scripts/patch-c5-${pair%%:*}.py" --input "$base/$payload" --output "$temporary/${pair%%:*}"
        install -m "$(stat -c %a "$base/$payload")" "$temporary/${pair%%:*}" "$vendor/$payload"
    done
    radio="$vendor/proprietary/vendor/lib64"
    snapshot="$ROOT/android/prebuilts/vndk/v29/arm64/arch-arm64-armv8-a/shared/vndk-core"
    cp "$snapshot/libprotobuf-cpp-full.so" "$radio/libprotobuf-c5-ril.so"
    for file in "$radio/libsettings.so" "$radio/libprotobuf-c5-ril.so"; do
        python3 scripts/patch-elf-dynstr.py "$file" libprotobuf-cpp-full.so=libprotobuf-c5-ril.so
    done
)
```

Use the BOOT input commands in the kernel build notes with `REFERENCE_BOOT=$ROOT/inputs/lineage-17.1/lineage-17.1-20261121-c5lte-boot.img`, `OUTPUT=$ROOT/kernel-build` and `BOOT_INPUTS=$ROOT/android/vendor/samsung/c5lte/boot`. The host environment is the saved Ubuntu 20.04 Android builder unpacked at `$ROOT/inputs/host/rootfs`. The local archive `ubuntu20-android-build.tar` has SHA-256 `a9996656bf12b3750126633d41203ad1f07b3b30275c4eaebe4bae6f06c78da3`. The image build mounts the Android checkout and host root read-only and writes into `$ROOT/android-out`, `$ROOT/build-home` and `$ROOT/tmp`.

```sh
./scripts/build.sh --lineage-version 18.1 --workdir "$ROOT/android" --target images --jobs 8
```

The product output directory is `$ROOT/android-out/target/product/c5lte`. The images target builds `boot.img`, `system.img` and `recovery.img`. To build just Lineage Recovery, use the same command with `--target recovery`. BOOT and RECOVERY use the same Linux 4.9.337 kernel and Rev09 device tree with separate Android 11 ramdisks. Recovery selects configfs and the `msm_hsusb` USB controller. The build checks the recovery executable, adbd, API level, Lineage version and recovery partition map in the packed ramdisk.

On the SM-C5000 the recovery menu displayed version 18.1 (20261123). After a fresh recovery boot, the previously authorized computer obtained an ADB shell with key authentication enabled and the “Enable ADB” menu action unused. The partition readback matched the installed recovery, and BOOT and SYSTEM retained their pre-installation hashes.

SYSTEM is an Android sparse image containing the Android 11 platform and the device-specific vendor payload. To prepare a raw copy for inspection, choose a new output path on the build disk.

```sh
product="$ROOT/android-out/target/product/c5lte"
raw="$ROOT/validation/system.raw.img"
mkdir -p "$ROOT/validation"
(
    set -eu
    test ! -e "$raw"
    simg2img "$product/system.img" "$raw"
    test "$(stat -c %s "$raw")" = 3196059648
    e2fsck -fn "$raw"
)
```

The Android 11 build exports separate ARM and arm64 userspace headers from the 4.9 source. This gives the 32-bit HALs and 64-bit processes their respective kernel ABI types. Its header-only Kbuild patch handles the C5 configuration with vServices disabled. The video HALs use the controls, events and extradata layout from the enabled C5 video driver in the kernel techpack.

The private amplifier library calls three ARM functions with the `LIBC_PRIVATE` symbol version. The ELF check uses the libc installed in the Android runtime APEX, which exports those versions. The build also checks the CSA1 GNSS files, camera HAL, sensor firmware and HAL, and both fingerprint adapters. The stock GNSS service requires the 32-bit `libhwbinder.so` through Samsung's HIDL interface library. Without it, the service exits before it can register and Android's location service blocks framework startup. The older 64-bit location API and data-service libraries required GPS libraries absent from the accepted 17.1 system.

The Android 11 fingerprint, USB, Wi-Fi, hostapd and supplicant services supply their own VINTF declarations. The build merges the installed service fragments with the C5 vendor manifest to detect conflicts before a device trial. Android's BatteryService also requires a registered Health HAL. The product installs the binderized Health 2.1 service and its matching passthrough implementation. The build checks the service executable, implementation, init entry and manifest fragment. The C5 policy labels the battery, fuel gauge and charger power-supply paths from the 4.9 kernel so the Health HAL can read them in enforcing mode. In off-mode charging, the C5 init service launches `/system/bin/charger` to display battery status and handle a long press of the power key. The 17.1 image supplied a `/charger` symlink at the path used by the original service definition. On the SM-C5000 with USB connected throughout, the charging screen appeared after shutdown and holding Power started Android 11.

On an SM-C5000 with the CSA1 bootloader, Android 11 completed boot with Linux 4.9.337 and SELinux enforcing. The framework started SystemUI and Launcher3, recognized the existing system user, registered the GNSS and Health services, and read the battery level and USB power state. Both radio daemon instances and the network manager started. In a later outdoor test, the Android GPS provider delivered a fresh fix after 16.798 seconds, with up to 20 satellites visible. Cellular calls and mobile data were skipped in the absence of a SIM.

The C5 lockscreen uses the Lineage USB restriction service backed by the kernel’s `deny_new_usb` control. Without the service, a synchronous lookup delays the lockscreen redraw. For declared binderized HALs, blocking HIDL lookups use the existing registration wait directly. The panel brightness node has the standard LED sysfs label. On the tested build, Android reported wake completion in 702 ms and the panel driver issued its first nonzero brightness command after 204 ms.

Once Android reports boot completion, HIDL skips the legacy one-second startup delay when probing optional services and loading passthrough implementations. This lets a new app process probe graphics mapper versions 4 and 3 and load the installed mapper 2.0 without three fixed delays before its first frame. On the SM-C5000, removing Settings from Recents and launching it again produced a new process in each of three trials. Time to the first frame fell from 6.25–6.58 seconds on the previous image to 1.17–1.21 seconds.

The C5 sensor service uses the Bosch input node at `/sys/devices/virtual/input/input2`. The device policy labels that subtree `sysfs_sensors`. On the tested 4.9 build, Android enumerated the same 13 sensor types as on 17.1, and applications received accelerometer, device orientation and geomagnetic rotation events. The DC vibration motor uses the timed-output HAL and a single 50 ms long-press pulse. Long-press vibration and playback through the 3.5 mm headphone jack were confirmed on the handset.

The final device overlay enables Android's NFC feedback for unsupported tags and read errors. The launcher test recorded the unsupported-tag message. A foreground app received the tag intent in a separate test. On this image the Settings homepage gained focus after a 1.698-second cold launch. The final BOOT and expanded SYSTEM digests and the hardware acceptance coverage are in the [18.1 release summary](../README.md#lineageos-181-for-the-sm-c5000).
