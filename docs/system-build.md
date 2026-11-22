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

The device policy series targets the pinned device checkout. Regenerate the vendor makefiles from the patched proprietary-files.txt using the device's setup-makefiles.sh. A clean replay reproduced Android.bp, Android.mk, BoardConfigVendor.mk and c5lte-vendor.mk from those inputs.

From a prepared Android checkout, run `./scripts/build-usb-init.sh "$ANDROID_WORK" "$OUTPUT_PARENT"` to generate the four accepted USB init files. The command prints the path of its new output directory. All four outputs are checked against the accepted image.

To package the verified full SYSTEM image under its release filename, run the following command from the output directory. The README lists the SHA-256 for the compressed file and the raw partition image.

```sh
gzip -n -6 -c "$VERIFIED_RAW_SYSTEM" > lineage-17.1-20261121-c5lte-system.img.gz
```

## Independent baseline build

A clean, offline `systemimage` build from the prepared 17.1 checkout completed on Linux. A file-by-file comparison with the archived October target-files SYSTEM found 5,101 identical entries out of 5,115. The comparison covered regular-file contents, type, mode, owner, group, symlink target and extended attributes.

The standalone system image omitted the two recovery-install files present in target-files. Twelve shared paths differed in content. Four of those files matched the accepted payloads exactly: Settings.apk, its arm64 odex and vdex, and vendor/etc/sec_config. The other eight were two build-property files, the license notice, the boot animation, Messaging, and Snap with its odex and vdex. All 480 boot-animation frames differed only in their PNG timestamp chunk. The fresh baseline image has a different full-image digest from the archived baseline. Use the accepted image hashes in the README when identifying a tested image.
