#!/usr/bin/env bash
set -euo pipefail

readonly stock_input_sha256=cacaac54ef5923a0e789a5bce0b2e2176009dd01a134d9623d505899e7f8f4b5
readonly stock_output_sha256=91390daccc07aeb211d12ebc4445d46fb23581d0c358c58a8e173247404b5f9b
readonly normalized_input_sha256=f6b0f51cb19ff8c964acbaefec0f21135cd3f680c2cb7d5f307dc4abb492e85d
readonly normalized_output_sha256=0a7718fbde73ac353e5780d95c8cc7bd2e51acc19fcf34d7114c797718ab648c
readonly expected_size=16200
readonly patch_offset=$((0x95c))
readonly expected_input_bytes='05 f1 08 08'
readonly expected_output_bytes='05 f1 04 08'

usage() {
  cat <<'EOF'
Usage: ./scripts/patch-pm-proxy-mdmdetect.sh --workdir /absolute/android-work

Adjusts the exact CSA1 pm-proxy modem-record base from the Android 26
libmdmdetect layout to the pinned Android 25 layout. It does not communicate
with a phone.
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

blob="$workdir/vendor/samsung/c5lte/proprietary/vendor/bin/pm-proxy"
if [[ ! -f "$blob" ]]; then
  echo "Missing imported CSA1 PeripheralManager proxy: $blob" >&2
  exit 1
fi

size=$(stat -c '%s' "$blob")
sha256=$(sha256sum "$blob" | cut -d' ' -f1)
bytes=$(od -An -tx1 -j "$patch_offset" -N 4 "$blob" | xargs)

if [[ "$size" != "$expected_size" ]]; then
  echo "PeripheralManager proxy size mismatch: expected $expected_size bytes, found $size" >&2
  exit 1
fi

if [[ ( "$sha256" == "$stock_output_sha256" || "$sha256" == "$normalized_output_sha256" ) && "$bytes" == "$expected_output_bytes" ]]; then
  echo "PRESENT c5lte pm-proxy libmdmdetect layout compatibility patch"
  exit 0
fi

case "$sha256" in
  "$stock_input_sha256")
    output_sha256=$stock_output_sha256
    ;;
  "$normalized_input_sha256")
    output_sha256=$normalized_output_sha256
    ;;
  *)
    output_sha256=
    ;;
esac

if [[ -z "$output_sha256" || "$bytes" != "$expected_input_bytes" ]]; then
  echo "PeripheralManager proxy input mismatch; refusing to patch $blob" >&2
  echo "Expected SHA-256: $stock_input_sha256 or $normalized_input_sha256" >&2
  echo "Expected bytes:  $expected_input_bytes" >&2
  echo "Actual SHA-256/bytes:   $sha256 / $bytes" >&2
  exit 1
fi

temporary=$(mktemp "${blob}.mdmdetect-layout.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT
cp --preserve=mode,timestamps "$blob" "$temporary"
printf '\x05\xf1\x04\x08' | dd of="$temporary" bs=1 seek="$patch_offset" conv=notrunc status=none

patched_size=$(stat -c '%s' "$temporary")
patched_sha256=$(sha256sum "$temporary" | cut -d' ' -f1)
patched_bytes=$(od -An -tx1 -j "$patch_offset" -N 4 "$temporary" | xargs)
if [[ "$patched_size" != "$expected_size" || "$patched_sha256" != "$output_sha256" || "$patched_bytes" != "$expected_output_bytes" ]]; then
  echo "PeripheralManager proxy patch verification failed; original file was not changed" >&2
  exit 1
fi

mv -- "$temporary" "$blob"
trap - EXIT
echo "PATCHED c5lte pm-proxy libmdmdetect record base: $sha256 -> $output_sha256"
