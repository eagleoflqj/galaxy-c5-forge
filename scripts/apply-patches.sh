#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir

usage() {
  cat <<'EOF'
Usage: ./scripts/apply-patches.sh --workdir /absolute/android-work [--lineage-version 16.0|17.1|18.1]

Applies the tracked c5lte source patches idempotently to the pinned Android
worktree. It fails if a patch matches neither the forward nor reverse state.
When the version is omitted, the pinned LineageOS manifest commit is detected.
EOF
}

workdir=
lineage_version=
while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
      shift 2
      ;;
    --lineage-version)
      lineage_version=${2:-}
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$workdir" || "$workdir" != /* || ! -d "$workdir/.repo" ]]; then
  echo "--workdir must name an initialized absolute Android worktree" >&2
  exit 2
fi

if [[ -z "$lineage_version" ]]; then
  manifest_revision=$(git -C "$workdir/.repo/manifests" rev-parse HEAD)
  case "$manifest_revision" in
    e56f933c20b428410d1c53f41e6c0986880cb362)
      lineage_version=16.0
      ;;
    c7b3d4b65b79e2e0d6a891547ad138e1fcdafb56)
      lineage_version=17.1
      ;;
    75e412bbb78aa0009405f696d0d3775e995e64ca)
      lineage_version=18.1
      ;;
    *)
      echo "Cannot infer the LineageOS version from manifest revision: $manifest_revision" >&2
      exit 2
      ;;
  esac
fi
if [[ "$lineage_version" == 18.1 ]]; then
  patch_root="$project_dir/patches"
  device=device/samsung/c5lte
  audio=hardware/qcom-caf/msm8952/audio

  apply_source_patch() {
    local source="$workdir/$2" patch="$patch_root/$1/$2/$3"
    shift 3
    if ! git -C "$source" apply --reverse --check "$@" "$patch" 2>/dev/null; then
      git -C "$source" apply --check "$@" "$patch"
      git -C "$source" apply "$@" "$patch"
    fi
  }

  install_source() {
    local source="$patch_root/$1" destination="$workdir/$2"
    if [[ -e "$destination" || -L "$destination" ]]; then
      cmp "$source" "$destination"
    else
      install -Dm644 "$source" "$destination"
    fi
  }

  # These local inputs are prepared separately from the platform manifest.
  if [[ -d "$workdir/kernel/samsung/c5lte" ]]; then
    apply_source_patch lineage-18.1 kernel/samsung/c5lte export-c5-kernel-headers.patch
  fi
  if [[ -d "$workdir/vendor/samsung/c5lte" ]]; then
    apply_source_patch lineage-18.1 vendor/samsung/c5lte adapt-vendor-build-definitions.patch
  fi
  apply_source_patch lineage-18.1 "$device" port-c5lte-to-android-11.patch
  apply_source_patch shared "$device" 0003-use-csa1-stock-gnss-stack.patch '--include=libshims/gnss_hidl/*'
  install_source shared/device/samsung/c5lte/lights/Android.mk "$device/lights/Android.mk"
  install_source shared/device/samsung/c5lte/lights/lights.c "$device/lights/lights.c"
  install_source shared/device/samsung/c5lte/rootdir/etc/init.c5.zram.rc "$device/rootdir/etc/init.c5.zram.rc"
  apply_source_patch lineage-18.1 hardware/samsung include-fingerprint-module.patch
  apply_source_patch shared hardware/samsung 0001-restore-bauth-fingerprint-hal.patch --exclude=Android.mk
  apply_source_patch lineage-18.1 bootable/recovery preserve-installed-recovery.patch
  apply_source_patch lineage-18.1 hardware/interfaces preserve-bluetooth-service-groups.patch
  apply_source_patch lineage-18.1 system/core restore-legacy-logmessage-constructor.patch
  apply_source_patch lineage-18.1 system/libhidl avoid-runtime-hal-startup-delays.patch
  apply_source_patch lineage-18.1 frameworks/av resolve-audio-policy-before-create-effect.patch
  apply_source_patch lineage-18.1 frameworks/base restore-system-user-type.patch
  apply_source_patch shared frameworks/base 0001-handle-null-fingerprint-error-text-in-keyguard.patch
  apply_source_patch shared packages/apps/Snap 0001-stop-preview-before-hdr-reconfiguration.patch
  apply_source_patch shared packages/apps/Settings 0001-refresh-usb-debugging-switch-on-setting-change.patch --include=src/com/android/settings/development/AdbPreferenceController.java
  apply_source_patch lineage-18.1 packages/apps/Settings register-adb-preference-observer.patch
  apply_source_patch lineage-18.1 vendor/lineage generate-kernel-headers-for-each-abi.patch
  apply_source_patch lineage-18.1 hardware/qcom-caf/msm8952/media use-c5-video-kernel-headers.patch
  apply_source_patch shared "$audio" 0001-route-concrete-bluetooth-sco-devices.patch
  apply_source_patch shared "$audio" card-discovery/card-discovery.patch
  install_source shared/hardware/qcom-caf/msm8952/audio/card-discovery/c5-snd-card-discovery.inc "$audio/hal/audio_extn/c5-snd-card-discovery.inc"
  exit 0
fi
if [[ "$lineage_version" != 16.0 && "$lineage_version" != 17.1 ]]; then
  echo "--lineage-version must be 16.0, 17.1 or 18.1" >&2
  exit 2
fi

apply_patch_set() {
  local source_dir=$1
  local patch_dir=$2
  local patch

  if [[ ! -d "$source_dir/.git" ]]; then
    echo "Missing Git project: $source_dir" >&2
    return 1
  fi

  shopt -s nullglob
  local patches=("$patch_dir"/*.patch)
  shopt -u nullglob
  if (( ${#patches[@]} == 0 )); then
    echo "No patches found under: $patch_dir" >&2
    return 1
  fi

  for patch in "${patches[@]}"; do
    if git -C "$source_dir" apply --check "$patch" 2>/dev/null; then
      git -C "$source_dir" apply "$patch"
      printf 'APPLIED %s\n' "${patch#"$project_dir/"}"
    elif git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
      printf 'PRESENT %s\n' "${patch#"$project_dir/"}"
    elif [[ "$lineage_version" == 17.1 && "$patch" == "$project_dir/patches/shared/device/samsung/c5lte/0003-use-csa1-stock-gnss-stack.patch" ]] && \
        git -C "$source_dir" apply --reverse --check --exclude=sepolicy/hal_gnss_default.te "$patch" 2>/dev/null && \
        grep -Fqx 'allow hal_gnss_default gps_data_file:dir create_dir_perms;' "$source_dir/sepolicy/hal_gnss_default.te" && \
        grep -Fqx 'allow hal_gnss_default gps_data_file:file create_file_perms;' "$source_dir/sepolicy/hal_gnss_default.te" && \
        grep -Fqx 'allow hal_gnss_default location_data_file:dir r_dir_perms;' "$source_dir/sepolicy/hal_gnss_default.te"; then
      printf 'PRESENT %s (17.1 extended GNSS policy)\n' "${patch#"$project_dir/"}"
    elif [[ "$lineage_version" == 17.1 && "$patch" == "$project_dir/patches/shared/device/samsung/c5lte/0001-use-device-verified-boot-parameters.patch" ]] && \
        grep -Fqx 'require version-bootloader=C5000ZHU1CRJ5|C5000ZCU1CSA1' "$source_dir/board-info.txt" && \
        grep -Fq -- "--ramdisk_offset \$(BOARD_RAMDISK_OFFSET)" "$source_dir/board/kernel.mk" && \
        grep -Fq -- "--tags_offset \$(BOARD_KERNEL_TAGS_OFFSET)" "$source_dir/board/kernel.mk" && \
        grep -Fq "    init.recovery.qcom.power.rc \\" "$source_dir/product/init.mk" && \
        grep -Fq 'LOCAL_MODULE       := init.recovery.qcom.power.rc' "$source_dir/rootdir/Android.mk" && \
        grep -Fq 'LOCAL_MODULE       := init.recovery.qcom.rc' "$source_dir/rootdir/Android.mk" && \
        grep -Fq 'import /init.recovery.qcom.power.rc' "$source_dir/rootdir/etc/init.recovery.qcom.rc" && \
        grep -Fq "symlink /dev/block/platform/soc.0/\${ro.boot.bootdevice} /dev/block/bootdevice" "$source_dir/rootdir/etc/init.recovery.qcom.rc"; then
      printf 'PRESENT %s (17.1 normalized module tags)\n' "${patch#"$project_dir/"}"
    elif [[ "$lineage_version" == 17.1 && "$patch" == "$project_dir/patches/lineage-17.1/vendor/samsung/c5lte/0001-drop-android-8-bluetooth-service.patch" ]] && \
        ! grep -Fq 'android.hardware.bluetooth@1.0-service-qti' "$source_dir/c5lte-vendor.mk" && \
        grep -Fq 'android.hardware.bluetooth@1.0-impl-qti.so' "$source_dir/c5lte-vendor.mk"; then
      printf 'PRESENT %s (regenerated vendor makefile)\n' "${patch#"$project_dir/"}"
    elif [[ "$lineage_version" == 17.1 && "$patch" == "$project_dir/patches/lineage-17.1/device/samsung/c5lte/0014-restore-stock-pm-proxy-voter.patch" ]] && \
        grep -Fqx 'vendor/bin/pm-proxy|1026129efa13447af42fec53177923a604758e86' "$source_dir/proprietary-files.txt" && \
        grep -Fq 'service per_proxy /vendor/bin/pm-proxy' "$source_dir/rootdir/etc/init.qcom.rc" && \
        grep -Fq 'on property:init.svc.per_mgr=running' "$source_dir/rootdir/etc/init.qcom.rc" && \
        grep -Fq '    start per_proxy' "$source_dir/rootdir/etc/init.qcom.rc"; then
      printf 'PRESENT %s (extended by late-restart patch)\n' "${patch#"$project_dir/"}"
    else
      printf 'CONFLICT %s\n' "${patch#"$project_dir/"}" >&2
      return 1
    fi
  done
}

apply_patch_set \
  "$workdir/device/samsung/c5lte" \
  "$project_dir/patches/shared/device/samsung/c5lte"

apply_patch_set \
  "$workdir/external/one-true-awk" \
  "$project_dir/patches/shared/external/one-true-awk"

if [[ "$lineage_version" == 16.0 ]]; then
  apply_patch_set \
    "$workdir/bootable/recovery" \
    "$project_dir/patches/lineage-16.0/bootable/recovery"
else
  apply_patch_set \
    "$workdir/packages/apps/Settings" \
    "$project_dir/patches/shared/packages/apps/Settings"

  apply_patch_set \
    "$workdir/packages/apps/Snap" \
    "$project_dir/patches/shared/packages/apps/Snap"

  apply_patch_set \
    "$workdir/device/samsung/c5lte" \
    "$project_dir/patches/lineage-17.1/device/samsung/c5lte"

  apply_patch_set \
    "$workdir/vendor/samsung/c5lte" \
    "$project_dir/patches/lineage-17.1/vendor/samsung/c5lte"

  apply_patch_set \
    "$workdir/hardware/samsung" \
    "$project_dir/patches/shared/hardware/samsung"

  "$project_dir/scripts/patch-camera-qservice.sh" --workdir "$workdir"

  pm_proxy="$workdir/vendor/samsung/c5lte/proprietary/vendor/bin/pm-proxy"
  if [[ -f "$pm_proxy" ]]; then
    "$project_dir/scripts/patch-pm-proxy-mdmdetect.sh" --workdir "$workdir"
  else
    echo "DEFERRED c5lte pm-proxy libmdmdetect patch until the hash-pinned stock proxy is imported"
  fi

  peripheral_client32="$workdir/vendor/samsung/c5lte/proprietary/vendor/lib/libperipheral_client.so"
  peripheral_client64="$workdir/vendor/samsung/c5lte/proprietary/vendor/lib64/libperipheral_client.so"
  if [[ -f "$peripheral_client32" && -f "$peripheral_client64" ]]; then
    "$project_dir/scripts/patch-peripheral-parcel-abi.sh" --workdir "$workdir"
  else
    echo "DEFERRED c5lte PeripheralManager Parcel ABI patch until both hash-pinned client blobs are imported"
  fi

  apply_patch_set \
    "$workdir/hardware/interfaces" \
    "$project_dir/patches/lineage-17.1/hardware/interfaces"

  apply_patch_set \
    "$workdir/system/core" \
    "$project_dir/patches/lineage-17.1/system/core"

  apply_patch_set \
    "$workdir/hardware/qcom-caf/msm8952/audio" \
    "$project_dir/patches/shared/hardware/qcom-caf/msm8952/audio"

  apply_patch_set \
    "$workdir/kernel/samsung/c5lte" \
    "$project_dir/patches/lineage-17.1/kernel/samsung/c5lte"

  recovery_adb="$workdir/bootable/recovery/install/adb_install.cpp"
  recovery_init="$workdir/bootable/recovery/etc/init.rc"
  recovery_main="$workdir/bootable/recovery/recovery_main.cpp"
  if ! grep -Fq 'We need to call SetUsbConfig() after forking minadbd service.' "$recovery_adb" || \
      ! grep -Fq 'if (!SetUsbConfig("sideload"))' "$recovery_adb" || \
      ! grep -Fq 'on property:sys.usb.config=sideload && property:sys.usb.ffs.ready=1 && property:sys.usb.configfs=1' "$recovery_init" || \
      ! grep -Fq 'if (!SetUsbConfig("none"))' "$recovery_main"; then
    echo "LineageOS 17.1 recovery lacks the verified upstream USB sequencing" >&2
    exit 1
  fi
  echo "UPSTREAM LineageOS 17.1 recovery provides verified FunctionFS USB sequencing"

  if ! grep -Fq 'setprop ro.adb.nonblocking_ffs 0' "$workdir/device/samsung/c5lte/rootdir/etc/init.recovery.qcom.rc"; then
    echo "LineageOS 17.1 c5lte recovery does not force legacy FunctionFS on Linux 3.10" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte legacy FunctionFS selection"

  if ! grep -Fq 'setprop ro.adb.nonblocking_ffs 0' "$workdir/device/samsung/c5lte/rootdir/etc/init.qcom.usb.rc"; then
    echo "LineageOS 17.1 c5lte system does not force legacy FunctionFS on Linux 3.10" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte system legacy FunctionFS selection"

  if ! grep -Fq "    debug.stagefright.ccodec=0 \\" "$workdir/device/samsung/c5lte/product/media.mk"; then
    echo "LineageOS 17.1 c5lte does not disable Codec2 on the incompatible legacy Gralloc stack" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte legacy Gralloc Codec2 compatibility selection"

  if ! grep -Fq 'setprop ro.adb.secure.recovery 0' "$workdir/device/samsung/c5lte/rootdir/etc/init.recovery.qcom.rc"; then
    echo "LineageOS 17.1 c5lte recovery does not enable unauthenticated recovery ADB by default" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte automatic recovery ADB"

  if grep -Fq '<name>android.hardware.cas</name>' "$workdir/device/samsung/c5lte/manifest.xml"; then
    echo "LineageOS 17.1 c5lte device manifest still conflicts with the CAS 1.1 service fragment" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte non-conflicting CAS manifest"

  power_manifest=$(sed -n '/<name>android.hardware.power<\/name>/,/<\/hal>/p' "$workdir/device/samsung/c5lte/manifest.xml")
  if ! grep -Fq '<version>1.2</version>' <<<"$power_manifest" || \
      ! grep -Fq 'android.hardware.power@1.2-service-qti' "$workdir/device/samsung/c5lte/product/qcom-perf.mk" || \
      grep -Fq 'android.hardware.power@1.1-service-qti' "$workdir/device/samsung/c5lte/product/qcom-perf.mk" || \
      ! grep -Fq 'LOCAL_C_INCLUDES := vendor/qcom/opensource/power' "$workdir/device/samsung/c5lte/power/Android.mk"; then
    echo "LineageOS 17.1 c5lte does not select the available Android 10 QTI power HAL" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte Android 10 QTI power HAL selection"

  if ! grep -Fq 'android.hardware.bluetooth@1.0-service' "$workdir/device/samsung/c5lte/product/qcom-bluetooth.mk" || \
      grep -Fq 'android.hardware.bluetooth@1.0-impl' "$workdir/device/samsung/c5lte/product/qcom-bluetooth.mk" || \
      grep -Fq 'vendor.bluetooth.status=on' "$workdir/device/samsung/c5lte/product/qcom-bluetooth.mk" || \
      grep -Fq 'android.hardware.bluetooth@1.0-service-qti' "$workdir/vendor/samsung/c5lte/c5lte-vendor.mk" || \
      ! grep -Fq 'android.hardware.bluetooth@1.0-impl-qti.so' "$workdir/vendor/samsung/c5lte/c5lte-vendor.mk" || \
      ! grep -Fq 'group bluetooth system wakelock oem_2901' "$workdir/hardware/interfaces/bluetooth/1.0/default/android.hardware.bluetooth@1.0-service.rc"; then
    echo "LineageOS 17.1 c5lte does not select the Android 10 service with the QTI Pronto implementation" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte Android 10 Bluetooth service and QTI Pronto implementation"

  if ! grep -Fqx 'vendor/bin/pm-proxy|1026129efa13447af42fec53177923a604758e86' "$workdir/device/samsung/c5lte/proprietary-files.txt" || \
      ! grep -Fq 'service per_proxy /vendor/bin/pm-proxy' "$workdir/device/samsung/c5lte/rootdir/etc/init.qcom.rc" || \
      ! grep -Fq 'on property:init.svc.per_mgr=running' "$workdir/device/samsung/c5lte/rootdir/etc/init.qcom.rc" || \
      ! grep -Fq 'on property:sys.boot_completed=1' "$workdir/device/samsung/c5lte/rootdir/etc/init.qcom.rc" || \
      ! grep -Fq '    restart per_proxy' "$workdir/device/samsung/c5lte/rootdir/etc/init.qcom.rc"; then
    echo "LineageOS 17.1 c5lte does not select and late-restart the pinned CSA1 PeripheralManager proxy" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte CSA1 PeripheralManager proxy voter and late restart"

  lights_product="$workdir/device/samsung/c5lte/product/lights.mk"
  lights_makefile="$workdir/hardware/qcom-caf/msm8952/display/liblight/Android.mk"
  lights_source="$workdir/hardware/qcom-caf/msm8952/display/liblight/lights.c"
  if ! grep -Eq '^[[:space:]]+lights\.msm8952$' "$lights_product" || \
      grep -Fq 'lights.MSM8952' "$lights_product" || \
      ! grep -Fq "LOCAL_MODULE := lights.\$(TARGET_BOARD_PLATFORM)" "$lights_makefile" || \
      ! grep -Fq '"/sys/class/leds/lcd-backlight/brightness"' "$lights_source"; then
    echo "LineageOS 17.1 c5lte lacks the case-correct QCOM backlight HAL selection" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte QCOM backlight HAL selection and verified LED node"

  fingerprint_hal="$workdir/hardware/samsung/fingerprint/bauth/fingerprint.c"
  fingerprint_makefile="$workdir/hardware/samsung/fingerprint/bauth/Android.mk"
  fingerprint_root_makefile="$workdir/hardware/samsung/Android.mk"
  if [[ ! -f "$fingerprint_hal" || ! -f "$fingerprint_makefile" ]] || \
      ! grep -Fq "include \$(SAM_ROOT)/fingerprint/Android.mk" "$fingerprint_root_makefile" || \
      ! grep -Fq "LOCAL_MODULE := fingerprint.\$(TARGET_BOARD_PLATFORM)" "$fingerprint_makefile" || \
      ! grep -Fq 'dlopen("libbauthserver.so", RTLD_NOW)' "$fingerprint_hal" || \
      ! grep -Fq '.module_api_version = FINGERPRINT_MODULE_API_VERSION_2_1' "$fingerprint_hal"; then
    echo "LineageOS 17.1 Samsung hardware lacks the c5lte bauth legacy fingerprint adapter" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 Samsung bauth legacy fingerprint adapter"

  if ! grep -Fq 'LogMessage(const char* file, unsigned int line, LogId id, LogSeverity severity, int error);' "$workdir/system/core/base/include/android-base/logging.h" || \
      ! grep -Fq ': LogMessage(file, line, id, severity, nullptr, error) {}' "$workdir/system/core/base/logging.cpp"; then
    echo "LineageOS 17.1 libbase lacks the legacy LogMessage constructor required by the QTI Bluetooth implementation" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 libbase legacy LogMessage ABI compatibility"

  audio_policy="$workdir/hardware/qcom-caf/msm8952/audio/configs/msm8952_32/audio_policy_configuration.xml"
  if grep -Fq 'AUDIO_DEVICE_OUT_ALL_SCO' "$audio_policy" || \
      grep -Fq 'sink="BT SCO All"' "$audio_policy" || \
      ! grep -Fq 'sink="BT SCO"' "$audio_policy" || \
      ! grep -Fq 'sink="BT SCO Headset"' "$audio_policy" || \
      ! grep -Fq 'sink="BT SCO Car Kit"' "$audio_policy"; then
    echo "LineageOS 17.1 msm8952 audio policy does not route the three concrete SCO devices" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 msm8952 concrete Bluetooth SCO routes"

  if ! grep -Fq '#define __NR_memfd_create 279' "$workdir/kernel/samsung/c5lte/include/uapi/asm-generic/unistd.h" || \
      ! grep -Fq '__SYSCALL(__NR_memfd_create, sys_memfd_create)' "$workdir/kernel/samsung/c5lte/include/uapi/asm-generic/unistd.h" || \
      ! grep -Fq '#define __NR_memfd_create 385' "$workdir/kernel/samsung/c5lte/arch/arm64/include/asm/unistd32.h" || \
      ! grep -Fq '#define __NR_compat_syscalls' "$workdir/kernel/samsung/c5lte/arch/arm64/include/asm/unistd.h" || \
      ! grep -Eq '^#define __NR_compat_syscalls[[:space:]]+386$' "$workdir/kernel/samsung/c5lte/arch/arm64/include/asm/unistd.h" || \
      ! grep -Fq 'SYSCALL_DEFINE2(memfd_create,' "$workdir/kernel/samsung/c5lte/mm/shmem.c"; then
    echo "LineageOS 17.1 kernel lacks the native or compat memfd_create backport" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 kernel memfd_create backport"

  poweroff_source="$workdir/kernel/samsung/c5lte/drivers/power/reset/msm-poweroff.c"
  if (( $(grep -Fc 'qpnp_pon_set_restart_reason(PON_RESTART_REASON_UNKNOWN);' "$poweroff_source") != 2 )) || \
      ! grep -Fq 'Do not let an earlier recovery request survive a normal restart.' "$poweroff_source" || \
      ! grep -Fq 'Shutdown must not preserve a stale recovery request in the PMIC.' "$poweroff_source"; then
    echo "LineageOS 17.1 kernel does not clear stale PMIC restart reasons for normal restart and shutdown" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 kernel stale PMIC restart-reason clearing"

  pmic_dts="$workdir/kernel/samsung/c5lte/arch/arm/boot/dts/samsung/msm8952/msm8952-sec-c5lte-chn-hk-r09.dts"
  if ! grep -Fq 'qcom,store-hard-reset-reason;' "$pmic_dts"; then
    echo "LineageOS 17.1 c5lte device tree does not enable PMIC restart-reason storage" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 c5lte PMIC restart-reason storage"

  if grep -Fq 'service flash_recovery /system/bin/install-recovery.sh' "$workdir/system/core/rootdir/init.rc" || \
      grep -Fq 'on property:persist.sys.recovery_update=true' "$workdir/system/core/rootdir/init.rc"; then
    echo "LineageOS 17.1 system can still rewrite the separately managed recovery partition" >&2
    exit 1
  fi
  echo "PRESENT LineageOS 17.1 recovery rewrite suppression"
fi
