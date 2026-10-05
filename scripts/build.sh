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

check_sensor_output() {
  local system_dir=$1
  local firmware="$system_dir/vendor/firmware/ram_patch.fw"
  local hal="$system_dir/vendor/lib/sensors.c5lte.so"
  local actual
  local missing_symbols
  local -a needed
  local -a expected_needed=(liblog.so libcutils.so libdl.so libutils.so libc++.so libc.so libm.so)
  local -a providers=(
    "$system_dir/lib/liblog.so"
    "$system_dir/lib/libcutils.so"
    "$system_dir/lib/libdl.so"
    "$system_dir/lib/libutils.so"
    "$system_dir/lib/libc++.so"
    "$system_dir/lib/libc.so"
    "$system_dir/lib/libm.so"
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
  if ! readelf --wide -Ws "$hal" | awk '$7 != "UND" && $8 == "HMI" { found=1 } END { exit !found }'; then
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
    <(readelf --wide -Ws "$hal" | awk '$7 == "UND" && $8 != "" { name=$8; sub(/@.*/, "", name); print name }' | sort -u) \
    <(for provider in "${providers[@]}"; do readelf --wide -Ws "$provider"; done | awk '$7 != "UND" && $8 != "" { name=$8; sub(/@.*/, "", name); print name }' | sort -u))
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

usage() {
  cat <<'EOF'
Usage: ./scripts/build.sh --workdir /absolute/android-work --target recovery|rom [--jobs N] [--installclean]

Builds the selected target in the pinned LineageOS 16 container. It does not
flash, boot, or otherwise communicate with a device.
EOF
}

workdir=
target=
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
log_file="${project_dir}/logs/build-${target}-$(date -u +%Y%m%dT%H%M%SZ).log"
echo "Writing build log to $log_file"

"${project_dir}/scripts/apply-patches.sh" --workdir "$workdir"

if [[ "$installclean" == true ]]; then
  "${project_dir}/scripts/lineage-env.sh" run "$workdir" \
    bash -lc "source build/envsetup.sh; lunch lineage_c5lte-userdebug; mka installclean"
fi

"${project_dir}/scripts/lineage-env.sh" run "$workdir" \
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
else
  shopt -s nullglob
  artifacts=("${product_out}"/lineage-*.zip)
  shopt -u nullglob
  if (( ${#artifacts[@]} == 0 )); then
    echo "Build completed without a LineageOS ZIP under: $product_out" >&2
    exit 1
  fi

  artifact=${artifacts[0]}
  for candidate in "${artifacts[@]:1}"; do
    if [[ "$candidate" -nt "$artifact" ]]; then
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
  check_boot_header "${product_out}/boot.img"
  check_gnss_output "${product_out}/system"
  check_sensor_output "${product_out}/system"
  echo "ROM ZIP integrity and c5lte metadata checks passed"
fi
