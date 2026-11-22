# Rebuild the Galaxy C5 kernel

The tested Linux host uses GNU userspace, Git and Bubblewrap. Use a persistent disk for source and output. The kernel consumes an Android Clang r353983c1 toolchain and the Android arm64 and arm32 GCC toolchains. The Clang revision is f3bab93ac0f11f0c348b278a657c4c7ac425d628. The arm64 GCC and arm32 GCC prebuilt revisions are a61b4b9ea2a5098dbf113999526978aec683753b and 0e7d16580dd5fb78734174d56887f1e681d0ee4a. The reference build used frozen copies of these tools.

Supply local Git object databases containing the following source commits. For an ordinary checkout, pass the path that `git rev-parse --absolute-git-dir` reports.

| Source | Commit |
|---|---|
| LineageOS/android_kernel_xiaomi_msm8937 | ce6e4e43e6d70f266e114e77a736dcd8e949f111 |
| vm03/android_kernel_samsung_c5lte | 71b335bed497bbe8b9754c9c986801a66212ce4f |
| Linux stable | 87a72e81764d2fc5411706c551462d61cdb97660 |

Place the original controller firmware under `tsp_zinitix/zt7538_c5.fw` and `abov/abov_ft1604_c5.fw` in a private firmware directory. The build checks their SHA-256 values. The source preparation preserves file modes and symlinks. Its source and build output arguments each name a new directory.

```sh
./scripts/prepare-kernel.sh "$QCOM_GIT_DIR" "$SAMSUNG_GIT_DIR" "$STABLE_GIT_DIR" "$SOURCE"
./scripts/build-kernel.sh "$SOURCE" "$CLANG" "$ARM64_GCC" "$ARM32_GCC" "$FIRMWARE" "$OUTPUT" 4
```

The compressed kernel is `$OUTPUT/build/arch/arm64/boot/Image.gz`. Its expected SHA-256 is `eaead9f9166a04e28b84d0cf8beef54661b99a34422a14d596d989050fdff347`. The build sets a fixed link order for the techpack drivers. The original Kbuild enumerated them in filesystem order, which changed the final kernel even when every compiled leaf object matched.

A complete BOOT also contains the ramdisk, Rev09 device tree, Android boot header, Samsung trailer and an existing partition tail. The verified repack preserved these components from the reference full BOOT. The resulting full-partition digest is in the project README.

To prepare the full BOOT on the host, supply the LineageOS 17.1 mkbootimg executable and the private full BOOT template pinned by the packer. The packer verifies both inputs, the compact result and the full result. The output must be a new file.

```sh
./scripts/package-boot.sh "$MKBOOTIMG" "$REFERENCE_FULL_BOOT" "$OUTPUT/build/arch/arm64/boot/Image.gz" "$OUTPUT/lineage-17.1-20261121-c5lte-boot.img"
```
