#!/usr/bin/env bash
set -euo pipefail

directory=${1:?Specify the CSA1 proprietary vendor library directory}
input="$directory/libmmqjpeg_codec.so"
output="$directory/libmmqjpeg_codec.c5.so"
stock=6494b8dc2aedc77e0eb9bea083cb0b85889e12f6a792f8675612afa3b196da4b
repaired=7f9aa04342e7d3007e61c2878a5b4b753655e8be616f57ac911d4b1d7e5e70e2
if ! printf '%s  %s\n' "$stock" "$input" | sha256sum --check --status; then
  echo "Expected the original CSA1 JPEG encoder at $input (SHA-256 $stock)" >&2
  exit 1
fi
if [[ -f "$output" ]] && printf '%s  %s\n' "$repaired" "$output" | sha256sum --check --status; then
  exit 0
fi
temporary=$(mktemp "$directory/.libmmqjpeg.XXXXXXXX")
trap 'rm -f -- "$temporary"' EXIT
cp -- "$input" "$temporary"

# The constructor zeros the embedded LP32 default mutex and condition variable.
# Keep that pair alive across input images and sessions. The encoder destructor
# joins its workers and destroys the pair. Each replaced call's result is unused.
for offset in 0x213f6 0x21400 0x231a0 0x231aa; do
  printf '\x00\xbf\x00\xbf' | dd of="$temporary" bs=1 seek="$((offset))" conv=notrunc status=none
done
printf '%s  %s\n' "$repaired" "$temporary" | sha256sum --check --status
chmod --reference="$input" "$temporary"
mv -fT -- "$temporary" "$output"
