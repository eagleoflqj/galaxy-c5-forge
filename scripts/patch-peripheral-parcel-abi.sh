#!/usr/bin/env bash
set -euo pipefail

readonly client32_input_sha256=e512dc7c91c1c69bccfd72ba8bdb343673a93e01c2e162b6aa197e65f219bb1f
readonly client32_output_sha256=6e4a7757ac7223f6c7b17cc6f01b02a6cb6c445f2e9896ed1d50c1567d513aa9
readonly client32_size=54856
readonly client64_stock_input_sha256=664580412f4538b02a31ca1d1da9ff594691521b60abb0ee1f1c2ed65da156cd
readonly client64_callback_only_sha256=33a8feb5f6c517d8f7a929329e605baf671c6ddf0572282ca645d40069319890
readonly client64_stock_output_sha256=8da798a5115187d57e66c5564ec2b9332595babed6cac06e0e9d6061f1272def
readonly client64_size=67680
# File offsets (ELF virtual addresses are offset + 0x5000). The first seven
# instructions repair the callback; the rest cover all six request methods.
# Keep frame-relative canaries, saved registers and 16-byte SP alignment intact.
readonly -a client64_specs=(
  '0x9c68:4:ff8303d1:ff0304d1'
  '0x9c78:4:e0c30191:e0030291'
  '0x9c9c:4:e0c30191:e0030291'
  '0x9ca4:4:e0c30191:e0030291'
  '0x9cb8:4:e2c30191:e2030291'
  '0x9cd0:4:e0c30191:e0030291'
  '0x9ce0:4:e0c30191:e0030291'
  '0xa094:4:ffc303d1:ff4304d1'
  '0xa0a4:4:e0030291:e0430291'
  '0xa0d8:4:e0030291:e0430291'
  '0xa0e0:4:e0030291:e0430291'
  '0xa0ec:4:e0030291:e0430291'
  '0xa124:4:e0030291:e0430291'
  '0xa174:4:e2030291:e2430291'
  '0xa1b4:4:e0030291:e0430291'
  '0xa1fc:4:ff8303d1:ff0304d1'
  '0xa20c:4:e0c30191:e0030291'
  '0xa230:4:e0c30191:e0030291'
  '0xa238:4:e0c30191:e0030291'
  '0xa250:4:e2c30191:e2030291'
  '0xa278:4:e0c30191:e0030291'
  '0xa2b8:4:ff8303d1:ff0304d1'
  '0xa2c8:4:e0c30191:e0030291'
  '0xa2ec:4:e0c30191:e0030291'
  '0xa2f4:4:e0c30191:e0030291'
  '0xa30c:4:e2c30191:e2030291'
  '0xa334:4:e0c30191:e0030291'
  '0xa374:4:ff8303d1:ff0304d1'
  '0xa384:4:e0c30191:e0030291'
  '0xa3a8:4:e0c30191:e0030291'
  '0xa3b0:4:e0c30191:e0030291'
  '0xa3c8:4:e2c30191:e2030291'
  '0xa3f0:4:e0c30191:e0030291'
  '0xa434:4:ff8303d1:ff0304d1'
  '0xa444:4:e0c30191:e0030291'
  '0xa46c:4:e0c30191:e0030291'
  '0xa474:4:e0c30191:e0030291'
  '0xa480:4:e0c30191:e0030291'
  '0xa498:4:e2c30191:e2030291'
  '0xa4c0:4:e0c30191:e0030291'
  '0xa504:4:ff8303d1:ff0304d1'
  '0xa514:4:e0c30191:e0030291'
  '0xa534:4:e0c30191:e0030291'
  '0xa548:4:e2c30191:e2030291'
  '0xa570:4:e0c30191:e0030291'
)

usage() {
  cat <<'EOF'
Usage: ./scripts/patch-peripheral-parcel-abi.sh --workdir /absolute/android-work
       ./scripts/patch-peripheral-parcel-abi.sh --verify-client64 /absolute/client.so

Expands the stack layout used by the exact CSA1 32-bit and 64-bit
libperipheral_client Binder proxy methods for Android 10's larger Parcel
object, including the 64-bit registration/vote requests and callback. Verification
mode is read-only. It does not communicate with a phone.
EOF
}

workdir=
verify_client64=
while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
      shift 2
      ;;
    --verify-client64)
      verify_client64=${2:-}
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

if [[ -n "$verify_client64" ]]; then
  if [[ -n "$workdir" || "$verify_client64" != /* || ! -f "$verify_client64" ]]; then
    echo "--verify-client64 requires an absolute existing file and no --workdir" >&2
    exit 2
  fi
elif [[ -z "$workdir" || "$workdir" != /* || ! -d "$workdir/.repo" ]]; then
  echo "--workdir must name an initialized absolute Android worktree" >&2
  exit 2
fi

read_bytes() {
  local file=$1
  local offset=$2
  local count=$3
  od -An -tx1 -j "$offset" -N "$count" "$file" | xargs
}

verify_client32_bytes() {
  local file=$1
  local mode=$2
  local offset count input output expected actual
  local -a specs=(
    '0x7d84:2:9fb0:a3b0'
    '0x7d8a:2:11ac:13ac'
    '0x7d98:2:1e90:2290'
    '0x7de0:2:11a8:13a8'
    '0x7dfa:4:dde92679:dde92a79'
    '0x7e14:2:11ac:13ac'
    '0x7e52:2:1e99:2299'
    '0x7e60:2:1fb0:23b0'
    '0x7e7c:2:9cb0:a0b0'
    '0x7e82:2:0eac:10ac'
    '0x7e8e:2:1b90:1f90'
    '0x7ede:2:1b99:1f99'
    '0x7eec:2:1cb0:20b0'
    '0x7f08:2:9cb0:a0b0'
    '0x7f0e:2:0eac:10ac'
    '0x7f1a:2:1b90:1f90'
    '0x7f6a:2:1b99:1f99'
    '0x7f78:2:1cb0:20b0'
    '0x7f94:2:9cb0:a0b0'
    '0x7f9a:2:0eac:10ac'
    '0x7fa6:2:1b90:1f90'
    '0x7ff6:2:1b99:1f99'
    '0x8004:2:1cb0:20b0'
    '0x8020:2:9cb0:a0b0'
    '0x8026:2:0eac:10ac'
    '0x8032:2:1b90:1f90'
    '0x8058:2:2299:2699'
    '0x808a:2:1b99:1f99'
    '0x8098:2:1cb0:20b0'
    '0x80b2:2:9cb0:a0b0'
    '0x80b8:2:0eac:10ac'
    '0x80c0:2:1b90:1f90'
    '0x8104:2:1b99:1f99'
    '0x8112:2:1cb0:20b0'
  )

  for spec in "${specs[@]}"; do
    IFS=: read -r offset count input output <<<"$spec"
    if [[ "$mode" == input ]]; then
      expected=$input
    else
      expected=$output
    fi
    actual=$(read_bytes "$file" "$((offset))" "$count" | tr -d ' ')
    [[ "$actual" == "$expected" ]] || return 1
  done
}

verify_client64_bytes() {
  local file=$1
  local mode=$2
  local spec offset count input output expected actual
  for spec in "${client64_specs[@]}"; do
    IFS=: read -r offset count input output <<<"$spec"
    expected=$input
    if [[ "$mode" == output ]] || [[ "$mode" == callback && $((offset)) -lt $((0xa000)) ]]; then
      expected=$output
    fi
    actual=$(read_bytes "$file" $((offset)) "$count" | tr -d ' ')
    [[ "$actual" == "$expected" ]] || return 1
  done
}

patch_client32() {
  local file=$1
  local size sha256 temporary patched_sha256

  size=$(stat -c '%s' "$file")
  sha256=$(sha256sum "$file" | cut -d' ' -f1)
  if [[ "$size" != "$client32_size" ]]; then
    echo "32-bit PeripheralManager client size mismatch: expected $client32_size bytes, found $size" >&2
    return 1
  fi
  if [[ "$sha256" == "$client32_output_sha256" ]] && verify_client32_bytes "$file" output; then
    echo "PRESENT c5lte 32-bit PeripheralManager Android 10 Parcel ABI patch"
    return 0
  fi
  if [[ "$sha256" != "$client32_input_sha256" ]] || ! verify_client32_bytes "$file" input; then
    echo "32-bit PeripheralManager client input mismatch; refusing to patch $file" >&2
    echo "Expected SHA-256: $client32_input_sha256" >&2
    echo "Actual SHA-256:   $sha256" >&2
    return 1
  fi

  temporary=$(mktemp "${file}.parcel-abi.XXXXXX")
  trap 'rm -f -- "$temporary"' RETURN
  cp --preserve=mode,timestamps "$file" "$temporary"
  printf '\xa3\xb0' | dd of="$temporary" bs=1 seek=$((0x7d84)) conv=notrunc status=none
  printf '\x13\xac' | dd of="$temporary" bs=1 seek=$((0x7d8a)) conv=notrunc status=none
  printf '\x22\x90' | dd of="$temporary" bs=1 seek=$((0x7d98)) conv=notrunc status=none
  printf '\x13\xa8' | dd of="$temporary" bs=1 seek=$((0x7de0)) conv=notrunc status=none
  printf '\xdd\xe9\x2a\x79' | dd of="$temporary" bs=1 seek=$((0x7dfa)) conv=notrunc status=none
  printf '\x13\xac' | dd of="$temporary" bs=1 seek=$((0x7e14)) conv=notrunc status=none
  printf '\x22\x99' | dd of="$temporary" bs=1 seek=$((0x7e52)) conv=notrunc status=none
  printf '\x23\xb0' | dd of="$temporary" bs=1 seek=$((0x7e60)) conv=notrunc status=none
  printf '\xa0\xb0' | dd of="$temporary" bs=1 seek=$((0x7e7c)) conv=notrunc status=none
  printf '\x10\xac' | dd of="$temporary" bs=1 seek=$((0x7e82)) conv=notrunc status=none
  printf '\x1f\x90' | dd of="$temporary" bs=1 seek=$((0x7e8e)) conv=notrunc status=none
  printf '\x1f\x99' | dd of="$temporary" bs=1 seek=$((0x7ede)) conv=notrunc status=none
  printf '\x20\xb0' | dd of="$temporary" bs=1 seek=$((0x7eec)) conv=notrunc status=none
  printf '\xa0\xb0' | dd of="$temporary" bs=1 seek=$((0x7f08)) conv=notrunc status=none
  printf '\x10\xac' | dd of="$temporary" bs=1 seek=$((0x7f0e)) conv=notrunc status=none
  printf '\x1f\x90' | dd of="$temporary" bs=1 seek=$((0x7f1a)) conv=notrunc status=none
  printf '\x1f\x99' | dd of="$temporary" bs=1 seek=$((0x7f6a)) conv=notrunc status=none
  printf '\x20\xb0' | dd of="$temporary" bs=1 seek=$((0x7f78)) conv=notrunc status=none
  printf '\xa0\xb0' | dd of="$temporary" bs=1 seek=$((0x7f94)) conv=notrunc status=none
  printf '\x10\xac' | dd of="$temporary" bs=1 seek=$((0x7f9a)) conv=notrunc status=none
  printf '\x1f\x90' | dd of="$temporary" bs=1 seek=$((0x7fa6)) conv=notrunc status=none
  printf '\x1f\x99' | dd of="$temporary" bs=1 seek=$((0x7ff6)) conv=notrunc status=none
  printf '\x20\xb0' | dd of="$temporary" bs=1 seek=$((0x8004)) conv=notrunc status=none
  printf '\xa0\xb0' | dd of="$temporary" bs=1 seek=$((0x8020)) conv=notrunc status=none
  printf '\x10\xac' | dd of="$temporary" bs=1 seek=$((0x8026)) conv=notrunc status=none
  printf '\x1f\x90' | dd of="$temporary" bs=1 seek=$((0x8032)) conv=notrunc status=none
  printf '\x26\x99' | dd of="$temporary" bs=1 seek=$((0x8058)) conv=notrunc status=none
  printf '\x1f\x99' | dd of="$temporary" bs=1 seek=$((0x808a)) conv=notrunc status=none
  printf '\x20\xb0' | dd of="$temporary" bs=1 seek=$((0x8098)) conv=notrunc status=none
  printf '\xa0\xb0' | dd of="$temporary" bs=1 seek=$((0x80b2)) conv=notrunc status=none
  printf '\x10\xac' | dd of="$temporary" bs=1 seek=$((0x80b8)) conv=notrunc status=none
  printf '\x1f\x90' | dd of="$temporary" bs=1 seek=$((0x80c0)) conv=notrunc status=none
  printf '\x1f\x99' | dd of="$temporary" bs=1 seek=$((0x8104)) conv=notrunc status=none
  printf '\x20\xb0' | dd of="$temporary" bs=1 seek=$((0x8112)) conv=notrunc status=none
  patched_sha256=$(sha256sum "$temporary" | cut -d' ' -f1)
  if [[ "$patched_sha256" != "$client32_output_sha256" ]] || ! verify_client32_bytes "$temporary" output; then
    echo "32-bit PeripheralManager Parcel ABI patch verification failed; original file was not changed" >&2
    return 1
  fi
  mv -- "$temporary" "$file"
  trap - RETURN
  echo "PATCHED c5lte 32-bit PeripheralManager Parcel ABI: $sha256 -> $patched_sha256"
}

patch_client64() {
  local file=$1
  local size sha256 input_mode temporary patched_sha256
  local spec offset count input output escaped byte

  size=$(stat -c '%s' "$file")
  sha256=$(sha256sum "$file" | cut -d' ' -f1)
  if [[ "$size" != "$client64_size" ]]; then
    echo "64-bit PeripheralManager client size mismatch: expected $client64_size bytes, found $size" >&2
    return 1
  fi
  if [[ "$sha256" == "$client64_stock_output_sha256" ]] && verify_client64_bytes "$file" output; then
    echo "PRESENT c5lte 64-bit PeripheralManager complete Android 10 Parcel ABI patch"
    return 0
  fi
  case "$sha256" in
    "$client64_stock_input_sha256")
      input_mode=input
      ;;
    "$client64_callback_only_sha256")
      input_mode=callback
      ;;
    *)
      input_mode=
      ;;
  esac
  if [[ -z "$input_mode" ]] || ! verify_client64_bytes "$file" "$input_mode"; then
    echo "64-bit PeripheralManager client input mismatch; refusing to patch $file" >&2
    echo "Expected pinned stock SHA-256: $client64_stock_input_sha256 or callback-only $client64_callback_only_sha256" >&2
    echo "Actual SHA-256:   $sha256" >&2
    return 1
  fi

  temporary=$(mktemp "${file}.parcel-abi.XXXXXX")
  trap 'rm -f -- "$temporary"' RETURN
  cp --preserve=mode,timestamps "$file" "$temporary"
  for spec in "${client64_specs[@]}"; do
    IFS=: read -r offset count input output <<<"$spec"
    escaped=
    for ((byte = 0; byte < ${#output}; byte += 2)); do
      escaped+="\\x${output:byte:2}"
    done
    printf '%b' "$escaped" | dd of="$temporary" bs=1 seek=$((offset)) conv=notrunc status=none
  done
  patched_sha256=$(sha256sum "$temporary" | cut -d' ' -f1)
  if [[ "$patched_sha256" != "$client64_stock_output_sha256" ]] || ! verify_client64_bytes "$temporary" output; then
    echo "64-bit PeripheralManager Parcel ABI patch verification failed; original file was not changed" >&2
    return 1
  fi
  mv -- "$temporary" "$file"
  trap - RETURN
  echo "PATCHED c5lte 64-bit PeripheralManager Parcel ABI: $sha256 -> $patched_sha256"
}

if [[ -n "$verify_client64" ]]; then
  if [[ $(stat -c '%s' "$verify_client64") != "$client64_size" ]] || \
      [[ $(sha256sum "$verify_client64" | cut -d' ' -f1) != "$client64_stock_output_sha256" ]] || \
      ! verify_client64_bytes "$verify_client64" output; then
    echo "64-bit PeripheralManager client lacks the complete reviewed Parcel ABI repair" >&2
    exit 1
  fi
  echo "VERIFIED c5lte 64-bit PeripheralManager callback and all six request Parcel layouts"
  exit 0
fi

client32="$workdir/vendor/samsung/c5lte/proprietary/vendor/lib/libperipheral_client.so"
client64="$workdir/vendor/samsung/c5lte/proprietary/vendor/lib64/libperipheral_client.so"
if [[ ! -f "$client32" || ! -f "$client64" ]]; then
  echo "Missing imported CSA1 PeripheralManager clients under $workdir/vendor/samsung/c5lte/proprietary/vendor" >&2
  exit 1
fi

patch_client32 "$client32"
patch_client64 "$client64"
