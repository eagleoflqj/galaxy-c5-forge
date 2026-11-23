#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir

check_boot_header() {
  local image=$1
  local image_info

  image_info=$(abootimg -i "$image")
  if ! grep -Eq '^[[:space:]]*ramdisk:[[:space:]]+0x82000000$' <<<"$image_info"; then
    echo "Unexpected ramdisk address in $image; expected 0x82000000" >&2
    return 1
  fi
  if ! grep -Eq '^[[:space:]]*tags:[[:space:]]+0x81e00000$' <<<"$image_info"; then
    echo "Unexpected tags address in $image; expected 0x81e00000" >&2
    return 1
  fi
  echo "Boot header uses device-verified ramdisk and tags addresses: $image"
}

check_android11_image() {
  local image=$1 ramdisk=$2 inputs=$3
  local size offset=2048 i
  local -a fields files lengths
  # The legacy header packs Android 11.0.0 and the 2024-02 patch month in one word.
  read -r -a fields < <(od --endian=little -An -j8 -N40 -tu4 -w40 "$image")
  if [[ $(head -c8 "$image") != 'ANDROID!' || ${#fields[@]} != 10 ]] ||
      (( fields[1] != 0x80008000 || fields[4] != 0 || fields[7] != 2048 ||
         fields[9] != (11 << 25 | 24 << 4 | 2) )); then
    echo "Unexpected C5 Android 11 header in $image" >&2
    return 1
  fi
  check_boot_header "$image" || return 1
  files=("$inputs/Image.gz" "$ramdisk" "$inputs/dt.img")
  lengths=("${fields[0]}" "${fields[2]}" "${fields[8]}")
  for i in "${!files[@]}"; do
    size=$(stat -c %s "${files[i]}")
    if (( size == 0 || size != lengths[i] )) ||
        ! cmp -s -i "$offset:0" -n "$size" "$image" "${files[i]}"; then
      echo "Incorrect payload in $image: ${files[i]}" >&2
      return 1
    fi
    offset=$((offset + (size + 2047) / 2048 * 2048))
  done
  size=$(stat -c %s "$image")
  if (( size != offset + 16 || size > 33554432 )) ||
      [[ $(tail -c16 "$image") != SEANDROIDENFORCE ]]; then
    echo "Incorrect size or Samsung trailer in $image" >&2
    return 1
  fi
}

check_updater_scope() {
  local artifact=$1
  local updater_script
  local block_targets
  local image_entries
  local transfer_list
  local max_block
  local -r system_partition_blocks=780288

  updater_script=$(unzip -p "$artifact" META-INF/com/google/android/updater-script)
  block_targets=$(grep -oE '/dev/block/bootdevice/by-name/[A-Za-z0-9._-]+' <<<"$updater_script" | sed 's#.*/##' | sort -u)
  if [[ "$block_targets" != $'boot\nsystem' ]]; then
    echo "ROM updater references unexpected block targets: ${block_targets//$'\n'/, }" >&2
    return 1
  fi
  if grep -Eq '(^|[^[:alnum:]_])(format|delete_recursive|wipe_block_device)\(' <<<"$updater_script"; then
    echo "ROM updater contains a destructive filesystem operation outside the system block update" >&2
    return 1
  fi

  image_entries=$(unzip -Z1 "$artifact" | grep -E '(^|/)[^/]+\.img$' | sort -u)
  if [[ "$image_entries" != boot.img ]]; then
    echo "ROM ZIP contains unexpected image entries: ${image_entries//$'\n'/, }" >&2
    return 1
  fi

  transfer_list=$(unzip -p "$artifact" system.transfer.list)
  max_block=$(awk 'NR > 4 { for (field=2; field<=NF; field++) { gsub(/,/, " ", $field); count=split($field, ranges, " "); for (range=3; range<=count; range+=2) if (ranges[range] > max) max=ranges[range] } } END { print max+0 }' <<<"$transfer_list")
  if (( max_block > system_partition_blocks )); then
    echo "System transfer list exceeds the verified partition: $max_block > $system_partition_blocks blocks" >&2
    return 1
  fi
  echo "ROM updater scope is limited to system/boot; transfer range fits $system_partition_blocks blocks"
}

check_recovery_rewrite_disabled() {
  local artifact=$1
  local root_dir=$2
  local boot_image=$3
  local packaged_boot_hash
  local built_boot_hash

  if grep -Fq 'service flash_recovery /system/bin/install-recovery.sh' "$root_dir/init.rc" || \
      grep -Fq 'on property:persist.sys.recovery_update=true' "$root_dir/init.rc"; then
    echo "Built system-root init can still rewrite the separately managed recovery partition" >&2
    return 1
  fi

  packaged_boot_hash=$(unzip -p "$artifact" boot.img | sha256sum | cut -d' ' -f1)
  built_boot_hash=$(sha256sum "$boot_image" | cut -d' ' -f1)
  if [[ "$packaged_boot_hash" != "$built_boot_hash" ]]; then
    echo "Packaged boot image does not match the validated build output" >&2
    return 1
  fi

  echo "System-root init has no recovery rewrite service; packaged boot hash matches build output"
}

check_power_hal_output() {
  local system_dir=$1
  local manifest="$system_dir/vendor/etc/vintf/manifest.xml"
  local manifest_power
  local service="$system_dir/vendor/bin/hw/android.hardware.power@1.2-service-qti"
  local service_rc="$system_dir/vendor/etc/init/android.hardware.power@1.2-service-qti.rc"

  if [[ ! -x "$service" ]]; then
    echo "Built system lacks the Android 10 QTI power HAL service: $service" >&2
    return 1
  fi
  if [[ ! -f "$service_rc" ]] || ! grep -Fq '/vendor/bin/hw/android.hardware.power@1.2-service-qti' "$service_rc"; then
    echo "Built system lacks the matching Android 10 QTI power HAL init service" >&2
    return 1
  fi
  manifest_power=$(sed -n '/<name>android.hardware.power<\/name>/,/<\/hal>/p' "$manifest")
  if ! grep -Fq '<version>1.2</version>' <<<"$manifest_power" || \
      ! grep -Fq '<name>IPower</name>' <<<"$manifest_power" || \
      ! grep -Fq '<instance>default</instance>' <<<"$manifest_power"; then
    echo "Built vendor manifest does not declare android.hardware.power@1.2::IPower/default" >&2
    return 1
  fi

  echo "Android 10 QTI power HAL binary, init service and VINTF declaration are present"
}

check_lights_output() {
  local system_dir=$1
  local arch hal needed provider candidate missing_symbols
  local service="$system_dir/vendor/bin/hw/android.hardware.light@2.0-service"
  local service_rc="$system_dir/vendor/etc/init/android.hardware.light@2.0-service.rc"
  local manifest="$system_dir/vendor/etc/vintf/manifest.xml"
  local -a providers

  if [[ ! -x "$service" || ! -f "$service_rc" ]] || \
      ! grep -Fq '/vendor/bin/hw/android.hardware.light@2.0-service' "$service_rc" || \
      ! grep -Fq 'interface android.hardware.light@2.0::ILight default' "$service_rc" || \
      ! sed -n '/<name>android.hardware.light<\/name>/,/<\/hal>/p' "$manifest" | grep -Fq '<version>2.0</version>'; then
    echo "Built system lacks the light 2.0 service, init interface or VINTF declaration" >&2
    return 1
  fi
  for arch in lib lib64; do
    hal="$system_dir/vendor/$arch/hw/lights.msm8952.so"
    if [[ ! -f "$hal" ]] || \
        ! readelf --wide --dyn-syms "$hal" | awk '$7 != "UND" && $8 == "HMI" { found=1 } END { exit !found }' || \
        ! strings "$hal" | grep -Fx '/sys/class/leds/lcd-backlight/brightness' >/dev/null; then
      echo "Built system lacks the $arch concrete backlight HAL, HMI export or verified node" >&2
      return 1
    fi
    if ! readelf -h "$hal" | grep -Eq "Class:[[:space:]]+ELF$([[ $arch == lib64 ]] && echo 64 || echo 32)"; then
      echo "Built $arch backlight HAL has the wrong ELF class" >&2
      return 1
    fi
    providers=()
    while IFS= read -r needed; do
      provider=
      for candidate in "$system_dir/vendor/$arch/$needed" "$system_dir/$arch/$needed" \
          "$system_dir/apex/com.android.runtime.release/$arch/bionic/$needed"; do
        if [[ -f "$candidate" ]]; then
          provider=$candidate
          break
        fi
      done
      if [[ -z "$provider" ]]; then
        echo "Built $arch backlight HAL lacks dependency: $needed" >&2
        return 1
      fi
      providers+=("$provider")
    done < <(readelf -d "$hal" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
    if (( ${#providers[@]} == 0 )); then
      echo "Built $arch backlight HAL has no declared shared dependencies" >&2
      return 1
    fi
    missing_symbols=$(comm -23 \
      <(readelf --wide -Ws "$hal" | awk '/^[[:space:]]*[0-9]+:/ { for (f=1; f<=NF; f++) if ($f == "UND" && f < NF) { name=$(f+1); sub(/@.*/, "", name); if (name != "") print name } }' | sort -u) \
      <(for provider in "${providers[@]}"; do readelf --wide -Ws "$provider"; done | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { f=($NF ~ /^\([0-9]+\)$/ ? NF-1 : NF); name=$f; sub(/@.*/, "", name); print name }' | sort -u))
    if [[ -n "$missing_symbols" ]]; then
      echo "Built $arch backlight HAL has unresolved symbols: $missing_symbols" >&2
      return 1
    fi
  done
  echo "Concrete QCOM backlight HALs exist for both ABIs with HMI, verified LED node and dependency closure"
}

check_bluetooth_hal_output() {
  local system_dir=$1
  local vendor_dir="$system_dir/vendor"
  local service="$vendor_dir/bin/hw/android.hardware.bluetooth@1.0-service"
  local impl="$vendor_dir/lib64/hw/android.hardware.bluetooth@1.0-impl-qti.so"
  local generic_impl="$vendor_dir/lib64/hw/android.hardware.bluetooth@1.0-impl.so"
  local vendor_lib="$vendor_dir/lib64/libbt-vendor.so"
  local service_rc="$vendor_dir/etc/init/android.hardware.bluetooth@1.0-service.rc"
  local missing_symbols
  local -a providers=(
    "$system_dir/lib64/libbase.so"
    "$system_dir/lib64/libcutils.so"
    "$system_dir/lib64/libhidlbase.so"
    "$system_dir/lib64/libhidltransport.so"
    "$system_dir/lib64/liblog.so"
    "$system_dir/lib64/libutils.so"
    "$vendor_dir/lib64/libbtnv.so"
    "$vendor_dir/lib64/libdiag.so"
    "$vendor_dir/lib64/libqmiservices.so"
    "$vendor_dir/lib64/libqmi_cci.so"
    "$system_dir/lib64/android.hardware.bluetooth@1.0.so"
    "$system_dir/lib64/libc++.so"
    "$system_dir/apex/com.android.runtime.release/lib64/bionic/libdl.so"
    "$system_dir/apex/com.android.runtime.release/lib64/bionic/libc.so"
    "$system_dir/apex/com.android.runtime.release/lib64/bionic/libm.so"
  )

  if [[ ! -x "$service" || ! -f "$impl" || ! -f "$vendor_lib" ]]; then
    echo "Built system lacks the Android 10 Bluetooth service, QTI implementation or QCOM vendor library" >&2
    return 1
  fi
  if [[ ! -f "$service_rc" ]] || ! grep -Fq '/vendor/bin/hw/android.hardware.bluetooth@1.0-service' "$service_rc"; then
    echo "Built system lacks the matching Android 10 Bluetooth init service" >&2
    return 1
  fi
  if ! grep -Fq 'group bluetooth system wakelock oem_2901' "$service_rc"; then
    echo "Android 10 Bluetooth service does not preserve the QTI implementation supplemental groups" >&2
    return 1
  fi
  if [[ -n $(find "$vendor_dir" -type f \( -name 'android.hardware.bluetooth@1.0-service-qti' -o -name 'android.hardware.bluetooth@1.0-service-qti.rc' \) -print -quit) ]]; then
    echo "Built system still contains the Android 8 QTI Bluetooth service" >&2
    return 1
  fi
  if [[ -e "$generic_impl" ]]; then
    echo "Built system contains the generic Bluetooth implementation alongside the selected QTI implementation" >&2
    return 1
  fi
  if ! strings "$impl" | grep -Fx '/dev/smd2' >/dev/null || \
      ! strings "$impl" | grep -Fx '/dev/smd3' >/dev/null || \
      ! strings "$impl" | grep -F 'Downloading NVM' >/dev/null; then
    echo "Built Bluetooth HAL does not retain the QTI Pronto SMD/NVM implementation" >&2
    return 1
  fi
  for provider in "${providers[@]}"; do
    if [[ ! -f "$provider" ]]; then
      echo "Built system lacks Bluetooth HAL dependency provider: $provider" >&2
      return 1
    fi
  done
  missing_symbols=$(comm -23 \
    <(readelf --wide -Ws "$impl" | awk '/^[[:space:]]*[0-9]+:/ { for (field=1; field<=NF; field++) if ($field == "UND" && field < NF) { name=$(field+1); sub(/@.*/, "", name); if (name != "") print name } }' | sort -u) \
    <(for provider in "${providers[@]}"; do readelf --wide -Ws "$provider"; done | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { field=($NF ~ /^\([0-9]+\)$/ ? NF-1 : NF); name=$field; sub(/@.*/, "", name); print name }' | sort -u))
  if [[ -n "$missing_symbols" ]]; then
    echo "Built Bluetooth HAL has unresolved symbols in its declared providers:" >&2
    echo "$missing_symbols" >&2
    return 1
  fi

  echo "Android 10 Bluetooth service exclusively loads the QTI Pronto SMD/NVM implementation with complete 64-bit ABI closure"
}

check_audio_policy_output() {
  local system_dir=$1
  local audio_policy="$system_dir/vendor/etc/audio_policy_configuration.xml"

  if [[ ! -f "$audio_policy" ]]; then
    echo "Built system lacks the msm8952 XML audio policy: $audio_policy" >&2
    return 1
  fi
  if grep -Fq 'AUDIO_DEVICE_OUT_ALL_SCO' "$audio_policy" || \
      grep -Fq 'sink="BT SCO All"' "$audio_policy" || \
      ! grep -Fq 'sink="BT SCO"' "$audio_policy" || \
      ! grep -Fq 'sink="BT SCO Headset"' "$audio_policy" || \
      ! grep -Fq 'sink="BT SCO Car Kit"' "$audio_policy"; then
    echo "Built msm8952 audio policy does not route the three concrete SCO devices" >&2
    return 1
  fi

  echo "Built msm8952 audio policy routes concrete Bluetooth SCO devices without the unsafe aggregate port"
}

check_system_usb_output() {
  local system_dir=$1
  local usb_init="$system_dir/vendor/etc/init/hw/init.qcom.usb.rc"

  if [[ ! -f "$usb_init" ]] || ! grep -Fq 'setprop ro.adb.nonblocking_ffs 0' "$usb_init"; then
    echo "Built system does not select legacy FunctionFS for Android 10 adbd on Linux 3.10" >&2
    return 1
  fi

  echo "Built system selects legacy FunctionFS for reliable ADB enumeration on Linux 3.10"
}

check_media_codec_output() {
  local system_dir=$1
  local build_prop="$system_dir/build.prop"

  if [[ ! -f "$build_prop" ]] || ! grep -Fqx 'debug.stagefright.ccodec=0' "$build_prop"; then
    echo "Built system does not disable Codec2 on the incompatible legacy Gralloc stack" >&2
    return 1
  fi
  if grep -E '^debug\.stagefright\.ccodec=' "$build_prop" | grep -Fvx 'debug.stagefright.ccodec=0' >/dev/null; then
    echo "Built system contains a conflicting Codec2 property" >&2
    return 1
  fi

  echo "Built system disables Codec2 while retaining the legacy OMX media path"
}

check_camera_output() {
  local system_dir=$1
  local camera_hal="$system_dir/lib/hw/camera.vendor.msm8952.so"
  local actual_sha256
  local patched_bytes

  if [[ ! -f "$camera_hal" ]]; then
    echo "Built system lacks the c5lte proprietary camera HAL" >&2
    return 1
  fi
  actual_sha256=$(sha256sum "$camera_hal" | cut -d' ' -f1)
  patched_bytes=$(od -An -tx1 -j $((0x3b578)) -N 4 "$camera_hal" | xargs)
  if [[ $(stat -c '%s' "$camera_hal") != 748696 || "$actual_sha256" != 8a8a1ba5e06e3419abfd94bb4a6ba818a87371b5177fb3465976190ea3dbacf9 || "$patched_bytes" != '00 20 70 47' ]]; then
    echo "Built camera HAL does not contain the verified display.qservice compatibility patch" >&2
    return 1
  fi
  echo "Camera HAL output contains the hash-pinned display.qservice compatibility patch"
}

check_fingerprint_output() {
  local system_dir=$1
  local runtime="$system_dir/apex/${2:-com.android.runtime.release}"
  local arch
  local hal
  local vendor_lib
  local missing_symbols
  local missing_vendor_symbols
  local exported_vendor_symbols
  local -a providers
  local -a expected_vendor_symbols=(
    ss_fingerprint_authenticate
    ss_fingerprint_cancel
    ss_fingerprint_close
    ss_fingerprint_enroll
    ss_fingerprint_get_auth_id
    ss_fingerprint_open
    ss_fingerprint_post_enroll
    ss_fingerprint_pre_enroll
    ss_fingerprint_remove
    ss_fingerprint_set_active_group
    ss_set_notify_callback
  )

  for arch in lib lib64; do
    hal="$system_dir/$arch/hw/fingerprint.msm8952.so"
    vendor_lib="$system_dir/$arch/libbauthserver.so"
    providers=(
      "$system_dir/$arch/libhardware.so"
      "$system_dir/$arch/liblog.so"
      "$system_dir/$arch/libc++.so"
      "$runtime/$arch/bionic/libc.so"
      "$runtime/$arch/bionic/libm.so"
      "$runtime/$arch/bionic/libdl.so"
    )

    if [[ ! -f "$hal" || ! -f "$vendor_lib" ]]; then
      echo "Built system lacks the $arch c5lte fingerprint adapter or bauth provider" >&2
      return 1
    fi
    if ! readelf --wide -Ws "$hal" | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { field=($NF ~ /^\([0-9]+\)$/ ? NF-1 : NF); if ($field == "HMI") found=1 } END { exit !found }'; then
      echo "Built $arch fingerprint adapter lacks the legacy HMI module export" >&2
      return 1
    fi
    for provider in "${providers[@]}"; do
      if [[ ! -f "$provider" ]]; then
        echo "Built system lacks fingerprint adapter dependency provider: $provider" >&2
        return 1
      fi
    done
    missing_symbols=$(comm -23 \
      <(readelf --wide -Ws "$hal" | awk '/^[[:space:]]*[0-9]+:/ { for (field=1; field<=NF; field++) if ($field == "UND" && field < NF) { name=$(field+1); sub(/@.*/, "", name); if (name != "") print name } }' | sort -u) \
      <(for provider in "${providers[@]}"; do readelf --wide -Ws "$provider"; done | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { field=($NF ~ /^\([0-9]+\)$/ ? NF-1 : NF); name=$field; sub(/@.*/, "", name); print name }' | sort -u))
    if [[ -n "$missing_symbols" ]]; then
      echo "Built $arch fingerprint adapter has unresolved symbols in its declared providers:" >&2
      echo "$missing_symbols" >&2
      return 1
    fi
    exported_vendor_symbols=$(readelf --wide -Ws "$vendor_lib" | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { name=$NF; if (name ~ /^ss_(fingerprint_|set_notify_callback$)/) print name }' | sort -u)
    missing_vendor_symbols=$(comm -23 \
      <(printf '%s\n' "${expected_vendor_symbols[@]}" | sort -u) \
      <(printf '%s\n' "$exported_vendor_symbols" | sort -u))
    if [[ -n "$missing_vendor_symbols" ]]; then
      echo "Built $arch bauth provider lacks adapter entry points:" >&2
      echo "$missing_vendor_symbols" >&2
      return 1
    fi
  done

  echo "c5lte bauth fingerprint adapters are present for both ABIs with complete link-time and runtime entry-point closure"
}

check_kernel_memfd_output() {
  local kernel_obj=$1
  local system_map="$kernel_obj/System.map"
  local config="$kernel_obj/.config"

  if [[ ! -f "$system_map" ]] || ! grep -Eq '[[:space:]]sys_memfd_create$' "$system_map"; then
    echo "Built kernel does not export the memfd_create syscall implementation" >&2
    return 1
  fi
  if [[ ! -f "$config" ]] || ! grep -Fqx 'CONFIG_TMPFS=y' "$config"; then
    echo "Built kernel lacks the tmpfs support required by memfd_create" >&2
    return 1
  fi

  echo "Built kernel contains sys_memfd_create with required tmpfs support"
}

check_kernel_ipa_shutdown_output() {
  local kernel_obj=$1
  local system_map="$kernel_obj/System.map"
  local vmlinux="$kernel_obj/vmlinux"
  local symbol

  for symbol in ipa_q6_pre_shutdown_cleanup ipa_q6_pre_shutdown_cleanup_work; do
    if [[ ! -f "$system_map" ]] || ! grep -Eq "[[:space:]]${symbol}$" "$system_map"; then
      echo "Built kernel lacks IPA shutdown implementation: $symbol" >&2
      return 1
    fi
  done
  if [[ ! -f "$vmlinux" ]] || ! strings "$vmlinux" | grep -F 'IPA Q6 cleanup in worker complete' >/dev/null; then
    echo "Built kernel lacks the signal-free IPA cleanup completion marker" >&2
    return 1
  fi

  echo "Built kernel contains the signal-free IPA shutdown worker (hardware regression still required)"
}

check_kernel_restart_reason_output() {
  local kernel_obj=$1
  local dtb="$kernel_obj/arch/arm64/boot/dts/msm8952-sec-c5lte-chn-hk-r09.dtb"
  local property_count

  if [[ ! -f "$dtb" ]]; then
    echo "Built kernel is missing the c5lte device tree: $dtb" >&2
    return 1
  fi
  property_count=$(strings "$dtb" | grep -Fxc 'qcom,store-hard-reset-reason' || true)
  if [[ "$property_count" != 1 ]]; then
    echo "Built c5lte device tree does not contain exactly one PMIC restart-reason storage property" >&2
    return 1
  fi

  echo "Built c5lte device tree enables PMIC restart-reason storage"
}

check_gnss_output() {
  local system_dir=$1
  local entries=0
  local mismatches=0
  local patched_entries=0
  local dst
  local expected
  local target
  local actual
  local dynamic
  local rewrite_sec=0
  local rewrite_gps=0
  local rewrite_loc=0
  declare -A patched_hashes=()

  while IFS='|' read -r dst expected; do
    patched_hashes["$dst"]=$expected
    patched_entries=$((patched_entries + 1))
  done < <(sed -n "/^patched_files=(/,/^)/p" "${project_dir}/scripts/import-stock-gnss.sh" | sed -n "s/^  '\([^']*\)'$/\1/p")
  if (( patched_entries != 26 )); then
    echo "Expected 26 rewritten GNSS ELF hashes, found $patched_entries" >&2
    return 1
  fi

  if [[ ! -f "$system_dir/vendor/bin/hw/vendor.samsung.hardware.gnss@1.0-service" ]]; then
    echo "Built system lacks the Samsung GNSS service" >&2
    return 1
  fi
  for obsolete in \
    "$system_dir/vendor/bin/hw/android.hardware.gnss@1.0-service" \
    "$system_dir/vendor/lib/hw/android.hardware.gnss@1.0-impl.so" \
    "$system_dir/vendor/lib/hw/gps.msm8952.so" \
    "$system_dir/vendor/lib/libsecnativefeature_csa1.so" \
    "$system_dir/vendor/lib/libgps.utils_csa1.so" \
    "$system_dir/vendor/lib/libloc_core_csa1.so"; do
    if [[ -e "$obsolete" ]]; then
      echo "Built system contains obsolete generic GNSS artifact: $obsolete" >&2
      return 1
    fi
  done

  while IFS='|' read -r _ dst expected; do
    target="$system_dir/$dst"
    entries=$((entries + 1))
    if [[ -n ${patched_hashes[$dst]:-} ]]; then
      expected=${patched_hashes[$dst]}
    fi
    if [[ ! -f "$target" ]]; then
      echo "Built system lacks pinned GNSS file: $dst" >&2
      mismatches=$((mismatches + 1))
      continue
    fi
    actual=$(sha256sum "$target" | cut -d' ' -f1)
    if [[ "$actual" != "$expected" ]]; then
      echo "Built GNSS file hash mismatch: $dst" >&2
      mismatches=$((mismatches + 1))
    fi
    if file "$target" | grep -q 'ELF'; then
      dynamic=$(readelf -d "$target" 2>/dev/null || true)
      if grep -Fq '[libsecnativefeature.so]' <<< "$dynamic" || grep -Fq '[libgps.utils.so]' <<< "$dynamic" || grep -Fq '[libloc_core.so]' <<< "$dynamic"; then
        echo "Built GNSS ELF retains a conflicting dependency name: $dst" >&2
        mismatches=$((mismatches + 1))
      fi
      grep -Fq '[libsecgnssfeature.so]' <<< "$dynamic" && rewrite_sec=$((rewrite_sec + 1))
      grep -Fq '[libgps.csa1.so]' <<< "$dynamic" && rewrite_gps=$((rewrite_gps + 1))
      grep -Fq '[libloc_c5.so]' <<< "$dynamic" && rewrite_loc=$((rewrite_loc + 1))
    fi
  done < <(sed -n "/^files=(/,/^)/p" "${project_dir}/scripts/import-stock-gnss.sh" | sed -n "s/^  '\([^']*\)'$/\1/p")

  if (( entries != 45 || mismatches != 0 || rewrite_sec != 7 || rewrite_gps != 25 || rewrite_loc != 19 )); then
    echo "GNSS output verification failed: entries=$entries mismatches=$mismatches rewrites=$rewrite_sec/$rewrite_gps/$rewrite_loc" >&2
    return 1
  fi
  if grep -Fq 'LD_PRELOAD' "$system_dir/vendor/etc/init/vendor.samsung.hardware.gnss@1.0-service.rc"; then
    echo "Samsung GNSS service still relies on LD_PRELOAD" >&2
    return 1
  fi
  echo "CSA1 GNSS output checks passed for 45 hash-pinned files and 26 rewritten ELFs"
}

check_gnss_peripheral_manager_policy_output() {
  local product_out=$1
  local policy_conf="${product_out}/obj/ETC/sepolicy_neverallows_intermediates/policy.conf"

  if [[ ! -f "$policy_conf" ]]; then
    echo "Built output lacks the expanded system SELinux policy source: $policy_conf" >&2
    return 1
  fi
  if ! grep -Fq 'allow hal_gnss_default servicemanager:binder { call transfer };' "$policy_conf" || \
      ! grep -Fq 'allow hal_gnss_default binder_per_mgr_service:service_manager find;' "$policy_conf" || \
      ! grep -Fq 'allow hal_gnss per_mgr:binder { call transfer };' "$policy_conf"; then
    echo "System SELinux policy does not connect the GNSS HAL to the legacy framework-binder PeripheralManager" >&2
    return 1
  fi

  echo "GNSS SELinux policy permits framework servicemanager discovery and PeripheralManager Binder calls"
}

check_peripheral_manager_proxy_output() {
  local product_out=$1
  local system_dir="$product_out/system"
  local proxy="$system_dir/vendor/bin/pm-proxy"
  local client32="$system_dir/vendor/lib/libperipheral_client.so"
  local client64="$system_dir/vendor/lib64/libperipheral_client.so"
  local init_rc="$system_dir/vendor/etc/init/hw/init.qcom.rc"
  local actual
  local -a needed
  local -a expected_needed=(libperipheral_client.so libcutils.so liblog.so libmdmdetect.so libc++.so libdl.so libc.so libm.so)

  if [[ ! -x "$proxy" ]] || ! file "$proxy" | grep -Fq 'ELF 32-bit LSB shared object, ARM'; then
    echo "Built system lacks the executable 32-bit CSA1 PeripheralManager proxy" >&2
    return 1
  fi
  actual=$(sha256sum "$proxy" | cut -d' ' -f1)
  if [[ "$actual" != 91390daccc07aeb211d12ebc4445d46fb23581d0c358c58a8e173247404b5f9b && \
        "$actual" != 0a7718fbde73ac353e5780d95c8cc7bd2e51acc19fcf34d7114c797718ab648c ]] || \
      [[ $(od -An -tx1 -j $((0x95c)) -N 4 "$proxy" | xargs) != '05 f1 04 08' ]]; then
    echo "Built CSA1 PeripheralManager proxy lacks the reviewed libmdmdetect layout patch: $actual" >&2
    return 1
  fi
  if [[ ! -f "$client32" ]] || [[ $(sha256sum "$client32" | cut -d' ' -f1) != 6e4a7757ac7223f6c7b17cc6f01b02a6cb6c445f2e9896ed1d50c1567d513aa9 ]] || \
      [[ $(od -An -tx1 -j $((0x7d84)) -N 2 "$client32" | xargs) != 'a3 b0' ]] || \
      [[ $(od -An -tx1 -j $((0x7dfa)) -N 4 "$client32" | xargs) != 'dd e9 2a 79' ]] || \
      [[ $(od -An -tx1 -j $((0x7e7c)) -N 2 "$client32" | xargs) != 'a0 b0' ]] || \
      [[ $(od -An -tx1 -j $((0x7f08)) -N 2 "$client32" | xargs) != 'a0 b0' ]] || \
      [[ $(od -An -tx1 -j $((0x7f94)) -N 2 "$client32" | xargs) != 'a0 b0' ]] || \
      [[ $(od -An -tx1 -j $((0x8020)) -N 2 "$client32" | xargs) != 'a0 b0' ]] || \
      [[ $(od -An -tx1 -j $((0x8058)) -N 2 "$client32" | xargs) != '26 99' ]] || \
      [[ $(od -An -tx1 -j $((0x80b2)) -N 2 "$client32" | xargs) != 'a0 b0' ]]; then
    echo "Built system lacks the reviewed 32-bit PeripheralManager Android 10 Parcel ABI patch" >&2
    return 1
  fi
  if [[ ! -f "$client64" ]]; then
    echo "Built system lacks the reviewed 64-bit PeripheralManager Android 10 Parcel ABI patch" >&2
    return 1
  fi
  if ! "$project_dir/scripts/patch-peripheral-parcel-abi.sh" --verify-client64 "$client64"; then
    echo "Built system lacks the complete 64-bit PeripheralManager Android 10 Parcel ABI patch" >&2
    return 1
  fi
  mapfile -t needed < <(readelf -d "$proxy" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
  if [[ "${needed[*]}" != "${expected_needed[*]}" ]]; then
    echo "Built CSA1 PeripheralManager proxy dependency set changed: ${needed[*]}" >&2
    return 1
  fi
  if [[ ! -f "$init_rc" ]] || ! grep -Fq 'service per_proxy /vendor/bin/pm-proxy' "$init_rc" || \
      ! grep -Fq 'on property:init.svc.per_mgr=running' "$init_rc" || \
      ! grep -Fq 'on property:sys.boot_completed=1' "$init_rc" || \
      ! grep -Fq '    restart per_proxy' "$init_rc"; then
    echo "Built init configuration does not start the proxy after pm-service and restart it after boot" >&2
    return 1
  fi

  echo "CSA1 PeripheralManager has the reviewed libmdmdetect layout patch, 32/64-bit Android 10 Parcel ABI patches and late init restart"
}

check_sensor_output() {
  local system_dir=$1
  local firmware="$system_dir/vendor/firmware/ram_patch.fw"
  local hal="$system_dir/vendor/lib/sensors.c5lte.so"
  local actual
  local missing_symbols
  local -a needed
  local -a expected_needed=(liblog.so libcutils.so libdl.so libutils.so libc++.so libc.so libm.so)
  local -a providers

  local bionic="$system_dir/lib"
  case "$lineage_version" in
    17.1) bionic="$system_dir/apex/com.android.runtime.release/lib/bionic" ;;
    18.1) bionic="$system_dir/apex/com.android.runtime/lib/bionic" ;;
  esac
  providers=(
    "$system_dir/lib/liblog.so"
    "$system_dir/lib/libcutils.so"
    "$bionic/libdl.so"
    "$system_dir/lib/libutils.so"
    "$system_dir/lib/libc++.so"
    "$bionic/libc.so"
    "$bionic/libm.so"
  )

  if [[ ! -f "$firmware" || ! -f "$hal" ]]; then
    echo "Built system lacks the matched CSA1 sensor firmware or HAL" >&2
    return 1
  fi

  actual=$(sha256sum "$firmware" | cut -d' ' -f1)
  if [[ "$actual" != ce23cc0006731977353a9d42168c5949783ff6a2882807a42969a65183136be4 || $(stat -c '%s' "$firmware") != 28476 ]]; then
    echo "Built system contains an unexpected sensor-hub firmware" >&2
    return 1
  fi
  actual=$(sha256sum "$hal" | cut -d' ' -f1)
  if [[ "$actual" != 96d03f6dc03202a510d2a75c7a39f72d58d871cbc5c8dfe104aaf818209a160a || $(stat -c '%s' "$hal") != 120508 ]]; then
    echo "Built system contains an unexpected sensor HAL" >&2
    return 1
  fi
  if ! file "$hal" | grep -Fq 'ELF 32-bit LSB shared object, ARM'; then
    echo "Built sensor HAL is not the expected 32-bit ARM ELF" >&2
    return 1
  fi
  mapfile -t needed < <(readelf -d "$hal" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
  if [[ "${needed[*]}" != "${expected_needed[*]}" ]]; then
    echo "Built sensor HAL has an unexpected dependency set: ${needed[*]}" >&2
    return 1
  fi
  if ! readelf --wide -Ws "$hal" | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { field=($NF ~ /^\([0-9]+\)$/ ? NF-1 : NF); name=$field; sub(/@.*/, "", name); if (name == "HMI") found=1 } END { exit !found }'; then
    echo "Built sensor HAL lacks the legacy HMI module export" >&2
    return 1
  fi
  for provider in "${providers[@]}"; do
    if [[ ! -f "$provider" ]]; then
      echo "Built system lacks sensor HAL dependency provider: $provider" >&2
      return 1
    fi
  done
  missing_symbols=$(comm -23 \
    <(readelf --wide -Ws "$hal" | awk '/^[[:space:]]*[0-9]+:/ { for (field=1; field<=NF; field++) if ($field == "UND") { name=$(field+1); sub(/@.*/, "", name); print name } }' | sort -u) \
    <(for provider in "${providers[@]}"; do readelf --wide -Ws "$provider"; done | awk '/^[[:space:]]*[0-9]+:/ && $0 !~ / UND / { field=($NF ~ /^\([0-9]+\)$/ ? NF-1 : NF); name=$field; sub(/@.*/, "", name); print name }' | sort -u))
  if [[ -n "$missing_symbols" ]]; then
    echo "Built sensor HAL has unresolved symbols in its declared providers:" >&2
    echo "$missing_symbols" >&2
    return 1
  fi
  if [[ -e "$system_dir/vendor/lib/hw/sensors.msm8952.so" ]]; then
    echo "Built system contains an unexpected duplicate legacy sensor HAL" >&2
    return 1
  fi

  echo "CSA1 sensor output checks passed for matched firmware/HAL hashes and 32-bit ABI closure"
}

check_recovery_param_policy_output() {
  local product_out=$1
  local recovery_policy_conf="${product_out}/obj/ETC/sepolicy.recovery_intermediates/sepolicy.recovery.conf"
  local system_policy_conf="${product_out}/obj/ETC/sepolicy_neverallows_intermediates/policy.conf"

  if [[ ! -f "$recovery_policy_conf" || ! -f "$system_policy_conf" ]]; then
    echo "Built output lacks the expected recovery or system SELinux policy source" >&2
    return 1
  fi
  if ! grep -Fq 'allow kernel block_device:dir search;' "$recovery_policy_conf" || \
      ! grep -Fq 'allow kernel block_device:lnk_file read;' "$recovery_policy_conf" || \
      ! grep -Eq '^allow kernel block_device:blk_file .*open.*read.*write' "$recovery_policy_conf"; then
    echo "Recovery SELinux policy does not permit the sec_param kernel worker path" >&2
    return 1
  fi
  if grep -Fq 'allow kernel block_device:dir search;' "$system_policy_conf" || \
      grep -Fq 'allow kernel block_device:lnk_file read;' "$system_policy_conf"; then
    echo "Recovery-only sec_param permissions leaked into the normal system policy" >&2
    return 1
  fi
  echo "Recovery-only sec_param SELinux policy checks passed"
}

usage() {
  cat <<'EOF'
Usage: ./scripts/build.sh --workdir /absolute/android-work --target recovery|rom|images [--lineage-version 16.0|17.1|18.1] [--jobs N] [--installclean]

Builds the selected target in the matching pinned LineageOS container. It does
not flash, boot, or otherwise communicate with a device. When the version is
omitted, the pinned LineageOS manifest commit is detected.
EOF
}

workdir=
target=
lineage_version=
jobs=$(getconf _NPROCESSORS_ONLN)
installclean=false

while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
      shift 2
      ;;
    --target)
      target=${2:-}
      shift 2
      ;;
    --lineage-version)
      lineage_version=${2:-}
      shift 2
      ;;
    --jobs)
      jobs=${2:-}
      shift 2
      ;;
    --installclean)
      installclean=true
      shift
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
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "--jobs must be a positive integer" >&2
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
  case "$target" in
    images) goals=(bootimage systemimage recoveryimage) ;;
    recovery) goals=(recoveryimage) ;;
    *) echo "Use --target images or recovery for LineageOS 18.1" >&2; exit 2 ;;
  esac
  workdir=$(realpath -e -- "$workdir")
  root=$(dirname -- "$workdir")
  output="$root/android-out"
  product_out="$output/target/product/c5lte"
  mkdir -p "$output" "$root/build-home" "$root/tmp" "$root/logs" "$workdir/out"
  for dir in "$workdir" "$output" "$root/tmp" "$root/build-home"; do
    case "$(findmnt -no FSTYPE -T "$dir")" in
      tmpfs|ramfs|'') echo "Select a disk-backed build directory: $dir" >&2; exit 2 ;;
    esac
  done
  boot_inputs="$workdir/vendor/samsung/c5lte/boot"
  (
    cd "$boot_inputs"
    sha256sum -c - <<'HASHES'
eaead9f9166a04e28b84d0cf8beef54661b99a34422a14d596d989050fdff347  Image.gz
f1d856f49b89eac54aaa0f5ca69091112297913e5611372c8affbc7d3168ee96  dt.img
HASHES
  )
  "${project_dir}/scripts/apply-patches.sh" --workdir "$workdir" --lineage-version 18.1
  # The build shell expands its own positional arguments inside the namespace.
  # shellcheck disable=SC2016
  bwrap --unshare-all --new-session --die-with-parent \
    --ro-bind "$root/inputs/host/rootfs" / \
    --overlay-src "$root/inputs/host/rootfs/etc" --tmp-overlay /etc \
    --symlink ../proc/self/mounts /etc/mtab --remount-ro /etc --proc /proc --dev /dev \
    --bind "$root/tmp" /tmp --bind "$root/build-home" /build-home \
    --ro-bind "$workdir" /android --bind "$output" /android/out \
    --chdir /android --clearenv --setenv PATH /usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    --setenv HOME /build-home --setenv LANG en_US.UTF-8 --setenv LC_ALL en_US.UTF-8 \
    --setenv BUILD_USERNAME forge --setenv BUILD_HOSTNAME galaxy-c5-forge \
    --setenv GIT_ALLOW_PROTOCOL https --setenv GIT_OPTIONAL_LOCKS 0 \
    /bin/bash -c '
      source build/envsetup.sh && lunch lineage_c5lte-userdebug || exit 1
      if [[ "$2" == true ]]; then m installclean || exit 1; fi
      jobs=$1
      shift 2
      m -j"$jobs" "$@"
    ' bash "$jobs" "$installclean" "${goals[@]}" 2>&1 | tee "$root/logs/build-18.1-$(date -u +%Y%m%dT%H%M%SZ).log"
  check_android11_image "$product_out/recovery.img" "$product_out/ramdisk-recovery.img" "$boot_inputs"
  recovery_root="$product_out/recovery/root"
  for file in prop.default init.recovery.qcom.rc system/bin/recovery system/bin/adbd system/etc/recovery.fstab; do
    gzip -dc "$product_out/ramdisk-recovery.img" | cpio -i --quiet --to-stdout "$file" | cmp - "$recovery_root/$file"
  done
  grep -Fxq 'ro.build.version.sdk=30' "$recovery_root/prop.default"
  grep -Eq '^ro.lineage.version=18\.1-.*-c5lte$' "$recovery_root/prop.default"
  grep -Fxq '    setprop sys.usb.configfs 1' "$recovery_root/init.recovery.qcom.rc"
  grep -Fxq '    setprop sys.usb.controller msm_hsusb' "$recovery_root/init.recovery.qcom.rc"
  for partition in boot system recovery; do
    grep -Eq "^/dev/block/bootdevice/by-name/${partition}[[:space:]]+/${partition}[[:space:]]" "$recovery_root/system/etc/recovery.fstab"
  done
  sha256sum "$product_out/recovery.img"
  if [[ "$target" == recovery ]]; then
    exit 0
  fi
  check_android11_image "$product_out/boot.img" "$product_out/ramdisk.img" "$boot_inputs"
  check_gnss_output "$product_out/system"
  if [[ ! -s "$product_out/system/lib/libhwbinder.so" ||
        ! -s "$product_out/system/lib64/libhwbinder.so" ||
        ! -s "$product_out/system/lib64/libshims_ril_audio.so" ]]; then
    echo "Missing HIDL or radio audio library required by the C5 vendor services" >&2
    exit 1
  fi
  (
    cd "$product_out/system/vendor/lib64"
    sha256sum -c - <<'HASHES'
b42958e97e65597a4ae046d9ff872b85a99259f8a1365acf8fb42d9bbca8d03b  libsettings.so
4af162edbbb20c2f18b869d699b0b52249a0ec8a23a953d3574c8d48d94488e0  libprotobuf-c5-ril.so
HASHES
  )
  check_sensor_output "$product_out/system"
  check_camera_output "$product_out/system"
  check_fingerprint_output "$product_out/system" com.android.runtime
  # The old device manifest and the Android 11 service fragments must load together.
  vintf_dir="$product_out/system/vendor/etc/vintf"
  vendor_manifests=("$vintf_dir/manifest.xml" "$vintf_dir"/manifest/*.xml)
  "$output/host/linux-x86/bin/assemble_vintf" -i "$(IFS=:; echo "${vendor_manifests[*]}")" -o /dev/null
  vendor_dir="$product_out/system/vendor"
  if [[ ! -x "$vendor_dir/bin/hw/android.hardware.health@2.1-service" ||
        ! -s "$vendor_dir/lib64/hw/android.hardware.health@2.0-impl-2.1.so" ]] ||
      ! grep -Fq '/vendor/bin/hw/android.hardware.health@2.1-service' "$vendor_dir/etc/init/android.hardware.health@2.1-service.rc" ||
      ! grep -Fq '@2.1::IHealth/default' "$vintf_dir/manifest/android.hardware.health@2.1.xml"; then
    echo "The Android 11 battery service requires the registered C5 Health HAL and its implementation" >&2
    exit 1
  fi
  sha256sum "$product_out/boot.img" "$product_out/system.img"
  exit 0
fi
if [[ "$lineage_version" != 16.0 && "$lineage_version" != 17.1 ]]; then
  echo "--lineage-version must be 16.0, 17.1 or 18.1" >&2
  exit 2
fi

case "$target" in
  recovery)
    build_goal=recoveryimage
    ;;
  rom)
    build_goal=bacon
    ;;
  *)
    echo "--target must be recovery or rom" >&2
    exit 2
    ;;
esac

mkdir -p "${project_dir}/logs"
log_file="${project_dir}/logs/build-lineage-${lineage_version}-${target}-$(date -u +%Y%m%dT%H%M%SZ).log"
echo "Writing build log to $log_file"

"${project_dir}/scripts/apply-patches.sh" --workdir "$workdir" --lineage-version "$lineage_version"

if [[ "$installclean" == true ]]; then
  LINEAGE_VERSION="$lineage_version" "${project_dir}/scripts/lineage-env.sh" run "$workdir" \
    bash -lc "source build/envsetup.sh; lunch lineage_c5lte-userdebug; mka installclean"
fi

LINEAGE_VERSION="$lineage_version" "${project_dir}/scripts/lineage-env.sh" run "$workdir" \
  bash -lc "set -o pipefail; source build/envsetup.sh; lunch lineage_c5lte-userdebug; mka -j${jobs} ${build_goal}" \
  2>&1 | tee "$log_file"

product_out="${workdir}/out/target/product/c5lte"
if [[ "$target" == recovery ]]; then
  artifact="${product_out}/recovery.img"
  if [[ ! -f "$artifact" ]]; then
    echo "Build completed without expected artifact: $artifact" >&2
    exit 1
  fi
  stat --printf='artifact=%n size=%s bytes\n' "$artifact"
  sha256sum "$artifact"
  check_boot_header "$artifact"
  python3 "${project_dir}/scripts/recovery-awk.py" check --image "$artifact"
  if [[ "$lineage_version" == 17.1 ]]; then
    check_kernel_restart_reason_output "${product_out}/obj/KERNEL_OBJ"
    check_kernel_ipa_shutdown_output "${product_out}/obj/KERNEL_OBJ"
    check_recovery_param_policy_output "$product_out"
  fi
else
  shopt -s nullglob
  artifacts=("${product_out}"/lineage-"${lineage_version}"-*-UNOFFICIAL-c5lte.zip)
  shopt -u nullglob
  if (( ${#artifacts[@]} == 0 )); then
    echo "Build completed without a LineageOS ZIP under: $product_out" >&2
    exit 1
  fi

  artifact=${artifacts[0]}
  for candidate in "${artifacts[@]:1}"; do
    if [[ "$candidate" -nt "$artifact" || ( ! "$candidate" -ot "$artifact" && "$candidate" > "$artifact" ) ]]; then
      artifact=$candidate
    fi
  done

  stat --printf='artifact=%n size=%s bytes\n' "$artifact"
  sha256sum "$artifact"
  unzip -t "$artifact" >/dev/null
  zip_entries=$(unzip -Z1 "$artifact")
  for required_entry in \
    META-INF/com/android/metadata \
    META-INF/com/google/android/updater-script \
    boot.img \
    system.new.dat.br \
    system.transfer.list; do
    if ! grep -Fqx "$required_entry" <<<"$zip_entries"; then
      echo "ROM ZIP lacks required entry: $required_entry" >&2
      exit 1
    fi
  done
  metadata=$(unzip -p "$artifact" META-INF/com/android/metadata)
  if ! grep -Fqx 'pre-device=c5lte' <<<"$metadata"; then
    echo "ROM metadata does not identify pre-device=c5lte" >&2
    exit 1
  fi
  check_updater_scope "$artifact"
  check_boot_header "${product_out}/boot.img"
  if [[ "$lineage_version" == 17.1 ]]; then
    check_recovery_rewrite_disabled "$artifact" "${product_out}/root" "${product_out}/boot.img"
    check_power_hal_output "${product_out}/system"
    check_lights_output "${product_out}/system"
    check_bluetooth_hal_output "${product_out}/system"
    check_audio_policy_output "${product_out}/system"
    check_system_usb_output "${product_out}/system"
    check_media_codec_output "${product_out}/system"
    check_camera_output "${product_out}/system"
    check_fingerprint_output "${product_out}/system"
    check_gnss_peripheral_manager_policy_output "$product_out"
    check_peripheral_manager_proxy_output "$product_out"
    check_kernel_memfd_output "${product_out}/obj/KERNEL_OBJ"
    check_kernel_restart_reason_output "${product_out}/obj/KERNEL_OBJ"
    check_kernel_ipa_shutdown_output "${product_out}/obj/KERNEL_OBJ"
  fi
  check_gnss_output "${product_out}/system"
  check_sensor_output "${product_out}/system"
  echo "ROM ZIP integrity and c5lte metadata checks passed"
fi
