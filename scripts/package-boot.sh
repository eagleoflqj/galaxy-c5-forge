#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $# != 4 ]]; then
    echo "Usage: $0 LINEAGE_MKBOOTIMG REFERENCE_FULL_BOOT IMAGE_GZ NEW_FULL_BOOT" >&2
    exit 2
fi
mkbootimg=$1
base=$2
kernel=$3
output=$4
require_hash() {
    local actual
    actual=$(sha256sum < "$1")
    [[ ${actual%% *} == "$2" ]] || { echo "SHA-256 mismatch: $1" >&2; exit 1; }
}
require_hash "$base" 2d0777933070dcfe449917ec9a3bab1d097119eba4055e49d70d0c2056d611c4
require_hash "$kernel" eaead9f9166a04e28b84d0cf8beef54661b99a34422a14d596d989050fdff347
scratch=$(mktemp -d -- "$(dirname -- "$output")/.c5-boot.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT

# The pinned template header fixes the sizes and aligned offsets of both sections.
dd if="$base" of="$scratch/ramdisk" bs=1M iflag=skip_bytes,count_bytes skip=10412032 count=793221 status=none
dd if="$base" of="$scratch/dt" bs=1M iflag=skip_bytes,count_bytes skip=11206656 count=244920 status=none
cmdline=$(dd if="$base" bs=1 skip=64 count=247 status=none)
"$mkbootimg" --kernel "$kernel" --ramdisk "$scratch/ramdisk" --dt "$scratch/dt" \
    --cmdline "$cmdline" --base 0x80000000 --kernel_offset 0x8000 --ramdisk_offset 0x02000000 \
    --second_offset 0x00f00000 --tags_offset 0x01e00000 --pagesize 2048 \
    --os_version 10.0.0 --os_patch_level 2023-02-01 -o "$scratch/compact"
printf '%s' SEANDROIDENFORCE >> "$scratch/compact"
require_hash "$scratch/compact" 4f87772c83e2ccaeca532ac1dfb91c7a5796f6cdb42f873bbd6fad1f79dae2b4
cp -- "$base" "$scratch/full"
dd if="$scratch/compact" of="$scratch/full" bs=1M conv=notrunc status=none
require_hash "$scratch/full" f7f70aa7ed89516a6698a26a4bb8dc5adb28d78f76dfc343e0bf6381e3f92357
ln -T -- "$scratch/full" "$output"
