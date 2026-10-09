#!/usr/bin/env bash
set -euo pipefail

readonly input_sha256=598351e23a581a0ad10f957ecdc32239816d948e9f6c29b8df550e130b79f8c9
readonly output_sha256=8a8a1ba5e06e3419abfd94bb4a6ba818a87371b5177fb3465976190ea3dbacf9
readonly expected_size=748696
readonly patch_offset=$((0x3b578))
readonly expected_input_bytes='2d e9 f0 4f'
readonly expected_output_bytes='00 20 70 47'

usage() {
  cat <<'EOF'
Usage: ./scripts/patch-camera-qservice.sh --workdir /absolute/android-work

Disables the obsolete display.qservice camera-status notification in the exact
c5lte proprietary camera HAL. It does not communicate with a phone.
EOF
}

workdir=
while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
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

blob="$workdir/vendor/samsung/c5lte/proprietary/lib/hw/camera.vendor.msm8952.so"
if [[ ! -f "$blob" ]]; then
  echo "Missing c5lte proprietary camera HAL: $blob" >&2
  exit 1
fi

size=$(stat -c '%s' "$blob")
sha256=$(sha256sum "$blob" | cut -d' ' -f1)
bytes=$(od -An -tx1 -j "$patch_offset" -N 4 "$blob" | xargs)

if [[ "$size" != "$expected_size" ]]; then
  echo "Camera HAL size mismatch: expected $expected_size bytes, found $size" >&2
  exit 1
fi

if [[ "$sha256" == "$output_sha256" && "$bytes" == "$expected_output_bytes" ]]; then
  echo "PRESENT c5lte camera HAL display.qservice compatibility patch"
  exit 0
fi

if [[ "$sha256" != "$input_sha256" || "$bytes" != "$expected_input_bytes" ]]; then
  echo "Camera HAL input mismatch; refusing to patch $blob" >&2
  echo "Expected SHA-256/bytes: $input_sha256 / $expected_input_bytes" >&2
  echo "Actual SHA-256/bytes:   $sha256 / $bytes" >&2
  exit 1
fi

temporary=$(mktemp "${blob}.camera-qservice.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT
cp --preserve=mode,timestamps "$blob" "$temporary"
printf '\x00\x20\x70\x47' | dd of="$temporary" bs=1 seek="$patch_offset" conv=notrunc status=none

patched_size=$(stat -c '%s' "$temporary")
patched_sha256=$(sha256sum "$temporary" | cut -d' ' -f1)
patched_bytes=$(od -An -tx1 -j "$patch_offset" -N 4 "$temporary" | xargs)
if [[ "$patched_size" != "$expected_size" || "$patched_sha256" != "$output_sha256" || "$patched_bytes" != "$expected_output_bytes" ]]; then
  echo "Camera HAL patch verification failed; original file was not changed" >&2
  exit 1
fi

mv -- "$temporary" "$blob"
trap - EXIT
echo "PATCHED c5lte camera HAL display.qservice notification: $input_sha256 -> $output_sha256"
