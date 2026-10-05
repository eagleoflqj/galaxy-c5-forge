#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir
readonly expected_image_size=3196059648
readonly expected_image_sha256=1a113c05150d765a5e8142eac2b3d4bb29b2eecbd776fd37092a6338678f50b6
readonly expected_firmware_sha256=ce23cc0006731977353a9d42168c5949783ff6a2882807a42969a65183136be4
readonly expected_hal_sha256=96d03f6dc03202a510d2a75c7a39f72d58d871cbc5c8dfe104aaf818209a160a

usage() {
  cat <<'EOF'
Usage: ./scripts/import-stock-sensors.sh --workdir /absolute/android-work --system-image /absolute/system.raw.ext4

Imports the hash-pinned CSA1 BHA250 sensor-hub firmware and matching legacy
sensor HAL from the exact raw stock system image into the Android worktree's
proprietary vendor directory. It does not communicate with or modify a phone.
EOF
}

workdir=
system_image=

while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
      shift 2
      ;;
    --system-image)
      system_image=${2:-}
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
if [[ -z "$system_image" || "$system_image" != /* || ! -f "$system_image" ]]; then
  echo "--system-image must name an absolute raw ext4 image" >&2
  exit 2
fi
if ! command -v debugfs >/dev/null 2>&1; then
  echo "debugfs is required to read the raw ext4 image" >&2
  exit 2
fi

actual_image_size=$(stat -c '%s' "$system_image")
actual_image_sha256=$(sha256sum "$system_image" | awk '{print $1}')
if [[ "$actual_image_size" != "$expected_image_size" || "$actual_image_sha256" != "$expected_image_sha256" ]]; then
  echo "Stock system image does not match C5000ZCU1CSA1 CHC" >&2
  echo "Expected: $expected_image_size bytes, SHA-256 $expected_image_sha256" >&2
  echo "Actual:   $actual_image_size bytes, SHA-256 $actual_image_sha256" >&2
  exit 1
fi

device_dir="$workdir/device/samsung/c5lte"
vendor_dir="$workdir/vendor/samsung/c5lte"
if [[ ! -d "$device_dir/.git" || ! -d "$vendor_dir/.git" ]]; then
  echo "Expected c5lte device and vendor Git projects are missing" >&2
  exit 2
fi

"$project_dir/scripts/apply-patches.sh" --workdir "$workdir"

if ! grep -Fqx 'vendor/firmware/ram_patch.fw|56524a279cbfa9fe1d21b88d55f3dd2beac84d2d' "$device_dir/proprietary-files.txt"; then
  echo "CSA1 sensor firmware manifest entry is not applied" >&2
  exit 1
fi
if ! grep -Fqx 'vendor/lib/hw/sensors.msm8952.so:vendor/lib/sensors.c5lte.so|f54326abc0c5d3b0f0aed67a05677ec2946572e9' "$device_dir/proprietary-files.txt"; then
  echo "CSA1 sensor HAL manifest entry is not applied" >&2
  exit 1
fi

stage_dir=$(mktemp -d /tmp/c5-csa1-sensors.XXXXXX)
trap 'rm -rf -- "$stage_dir"' EXIT

files=(
  "/vendor/firmware/ram_patch.fw|vendor/firmware/ram_patch.fw|$expected_firmware_sha256"
  "/vendor/lib/hw/sensors.msm8952.so|vendor/lib/sensors.c5lte.so|$expected_hal_sha256"
)

for record in "${files[@]}"; do
  IFS='|' read -r source_path target_path expected_sha256 <<< "$record"
  staged="$stage_dir/$target_path"
  install -d -m 700 "$(dirname "$staged")"
  debugfs -R "dump -p $source_path $staged" "$system_image" >/dev/null 2>&1
  actual_sha256=$(sha256sum "$staged" | awk '{print $1}')
  if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "Hash mismatch for $source_path" >&2
    echo "Expected SHA-256: $expected_sha256" >&2
    echo "Actual SHA-256:   $actual_sha256" >&2
    exit 1
  fi
done

firmware="$stage_dir/vendor/firmware/ram_patch.fw"
hal="$stage_dir/vendor/lib/sensors.c5lte.so"
if [[ $(stat -c '%s' "$firmware") != 28476 ]]; then
  echo "Unexpected CSA1 sensor firmware size" >&2
  exit 1
fi
if [[ $(stat -c '%s' "$hal") != 120508 ]]; then
  echo "Unexpected CSA1 sensor HAL size" >&2
  exit 1
fi
if ! file "$hal" | grep -Fq 'ELF 32-bit LSB shared object, ARM'; then
  echo "CSA1 sensor HAL is not the expected 32-bit ARM ELF" >&2
  exit 1
fi

mapfile -t needed < <(readelf -d "$hal" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
expected_needed=(liblog.so libcutils.so libdl.so libutils.so libc++.so libc.so libm.so)
if [[ "${needed[*]}" != "${expected_needed[*]}" ]]; then
  echo "Unexpected CSA1 sensor HAL dependency set: ${needed[*]}" >&2
  exit 1
fi
if ! readelf --wide -Ws "$hal" | awk '$7 != "UND" && $8 == "HMI" { found=1 } END { exit !found }'; then
  echo "CSA1 sensor HAL does not export the legacy HMI module" >&2
  exit 1
fi

for record in "${files[@]}"; do
  IFS='|' read -r source_path target_path expected_sha256 <<< "$record"
  target="$vendor_dir/proprietary/$target_path"
  install -d -m 755 "$(dirname "$target")"
  install -m 0644 "$stage_dir/$target_path" "$target"
  actual_sha256=$(sha256sum "$target" | awk '{print $1}')
  if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "Post-install hash mismatch for $target" >&2
    exit 1
  fi
done

"$device_dir/setup-makefiles.sh"

if ! grep -Fq 'proprietary/vendor/firmware/ram_patch.fw:system/vendor/firmware/ram_patch.fw' "$vendor_dir/c5lte-vendor.mk"; then
  echo "Generated vendor makefile lacks the sensor firmware mapping" >&2
  exit 1
fi
if ! grep -Fq 'proprietary/vendor/lib/sensors.c5lte.so:system/vendor/lib/sensors.c5lte.so' "$vendor_dir/c5lte-vendor.mk"; then
  echo "Generated vendor makefile lacks the sensor HAL mapping" >&2
  exit 1
fi

printf 'Imported and verified the matched CSA1 sensor firmware and HAL from %s\n' "$system_image"
printf 'Regenerated proprietary makefiles under %s\n' "$vendor_dir"
