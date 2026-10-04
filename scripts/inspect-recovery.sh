#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./scripts/inspect-recovery.sh --image /absolute/recovery.img \
  [--partition-size BYTES] [--expected-sha256 HEX]

Performs read-only structural checks on an Android boot/recovery image. The
default partition-size candidate is 33554432 bytes for c5lte; it is not a
substitute for stock firmware, PIT, or device verification.
EOF
}

image=
partition_size=33554432
expected_sha256=

while (( $# > 0 )); do
  case "$1" in
    --image)
      image=${2:-}
      shift 2
      ;;
    --partition-size)
      partition_size=${2:-}
      shift 2
      ;;
    --expected-sha256)
      expected_sha256=${2:-}
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

if [[ -z "$image" || "$image" != /* || ! -f "$image" ]]; then
  echo "--image must name an existing absolute file" >&2
  exit 2
fi
if [[ ! "$partition_size" =~ ^[1-9][0-9]*$ ]]; then
  echo "--partition-size must be a positive integer" >&2
  exit 2
fi
if [[ -n "$expected_sha256" && ! "$expected_sha256" =~ ^[0-9a-fA-F]{64}$ ]]; then
  echo "--expected-sha256 must contain exactly 64 hexadecimal characters" >&2
  exit 2
fi
for command_name in abootimg file sha256sum stat; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required command is missing: $command_name" >&2
    exit 2
  fi
done

failures=0
warnings=0
size=$(stat --printf='%s' "$image")
actual_sha256=$(sha256sum "$image" | awk '{print $1}')
magic=$(head -c 8 "$image")

printf 'image=%s\nsize=%s bytes\npartition_size_candidate=%s bytes\nsha256=%s\n' \
  "$image" "$size" "$partition_size" "$actual_sha256"

if (( size > partition_size )); then
  printf 'FAIL image exceeds partition-size candidate by %d bytes\n' \
    "$((size - partition_size))"
  failures=$((failures + 1))
else
  printf 'PASS image is %d bytes below partition-size candidate\n' \
    "$((partition_size - size))"
fi

if [[ "$magic" == 'ANDROID!' ]]; then
  echo 'PASS Android boot image magic'
else
  printf 'FAIL unexpected image magic: %q\n' "$magic"
  failures=$((failures + 1))
fi

if [[ -n "$expected_sha256" ]]; then
  if [[ "${actual_sha256,,}" == "${expected_sha256,,}" ]]; then
    echo 'PASS expected SHA-256'
  else
    printf 'FAIL SHA-256 mismatch: expected %s\n' "${expected_sha256,,}"
    failures=$((failures + 1))
  fi
fi

if [[ "$(tail -c 16 "$image")" == 'SEANDROIDENFORCE' ]]; then
  echo 'PASS Samsung SEANDROIDENFORCE tail marker'
else
  echo 'WARN Samsung SEANDROIDENFORCE tail marker is absent'
  warnings=$((warnings + 1))
fi

echo
file "$image"
echo
if ! abootimg -i "$image"; then
  echo 'FAIL abootimg could not parse the image'
  failures=$((failures + 1))
fi

echo
printf 'Result: %d failure(s), %d warning(s)\n' "$failures" "$warnings"
(( failures == 0 ))
