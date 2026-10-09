#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir
readonly expected_image_size=3196059648
readonly expected_image_sha256=1a113c05150d765a5e8142eac2b3d4bb29b2eecbd776fd37092a6338678f50b6
readonly expected_proxy_size=16200
readonly expected_proxy_sha256=cacaac54ef5923a0e789a5bce0b2e2176009dd01a134d9623d505899e7f8f4b5

usage() {
  cat <<'EOF'
Usage: ./scripts/import-stock-pm-proxy.sh --workdir /absolute/android-work --system-image /absolute/system.raw.ext4

Imports the hash-pinned 32-bit CSA1 PeripheralManager proxy from the exact raw
stock system image. It does not communicate with or modify a phone.
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
for tool in debugfs file git readelf sha256sum stat; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required host tool is missing: $tool" >&2
    exit 2
  fi
done

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

"$project_dir/scripts/apply-patches.sh" --workdir "$workdir" --lineage-version 17.1
if ! grep -Fqx 'vendor/bin/pm-proxy|1026129efa13447af42fec53177923a604758e86' "$device_dir/proprietary-files.txt"; then
  echo "CSA1 PeripheralManager proxy manifest entry is not applied" >&2
  exit 1
fi

stage_dir=$(mktemp -d /tmp/c5-csa1-pm-proxy.XXXXXX)
trap 'rm -rf -- "$stage_dir"' EXIT
staged_proxy="$stage_dir/pm-proxy"
debugfs -R "dump -p /vendor/bin/pm-proxy $staged_proxy" "$system_image" >/dev/null 2>&1

actual_proxy_size=$(stat -c '%s' "$staged_proxy")
actual_proxy_sha256=$(sha256sum "$staged_proxy" | awk '{print $1}')
actual_proxy_git_hash=$(git hash-object "$staged_proxy")
if [[ "$actual_proxy_size" != "$expected_proxy_size" || "$actual_proxy_sha256" != "$expected_proxy_sha256" || "$actual_proxy_git_hash" != 1026129efa13447af42fec53177923a604758e86 ]]; then
  echo "Extracted CSA1 PeripheralManager proxy does not match the pinned binary" >&2
  echo "Expected: $expected_proxy_size bytes, SHA-256 $expected_proxy_sha256, Git blob 1026129efa13447af42fec53177923a604758e86" >&2
  echo "Actual:   $actual_proxy_size bytes, SHA-256 $actual_proxy_sha256, Git blob $actual_proxy_git_hash" >&2
  exit 1
fi
if ! file "$staged_proxy" | grep -Fq 'ELF 32-bit LSB shared object, ARM'; then
  echo "CSA1 PeripheralManager proxy is not the expected 32-bit ARM ELF" >&2
  exit 1
fi

mapfile -t needed < <(readelf -d "$staged_proxy" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
expected_needed=(libperipheral_client.so libcutils.so liblog.so libmdmdetect.so libc++.so libdl.so libc.so libm.so)
if [[ "${needed[*]}" != "${expected_needed[*]}" ]]; then
  echo "Unexpected CSA1 PeripheralManager proxy dependency set: ${needed[*]}" >&2
  exit 1
fi

target="$vendor_dir/proprietary/vendor/bin/pm-proxy"
install -d -m 755 "$(dirname "$target")"
install -m 0755 "$staged_proxy" "$target"
if [[ $(sha256sum "$target" | awk '{print $1}') != "$expected_proxy_sha256" ]]; then
  echo "Post-install hash mismatch for $target" >&2
  exit 1
fi
"$project_dir/scripts/patch-pm-proxy-mdmdetect.sh" --workdir "$workdir"
"$project_dir/scripts/patch-peripheral-parcel-abi.sh" --workdir "$workdir"

"$device_dir/setup-makefiles.sh"
if ! grep -Eq 'proprietary/vendor/bin/pm-proxy:(system/vendor|\$\(TARGET_COPY_OUT_VENDOR\))/bin/pm-proxy' "$vendor_dir/c5lte-vendor.mk"; then
  echo "Generated vendor makefile lacks the CSA1 PeripheralManager proxy mapping" >&2
  exit 1
fi

printf 'Imported and verified the 32-bit CSA1 PeripheralManager proxy from %s\n' "$system_image"
printf 'Regenerated proprietary makefiles under %s\n' "$vendor_dir"
