#!/usr/bin/env bash
set -euo pipefail

failures=0

check_ref() {
  local label=$1
  local url=$2
  local ref=$3
  local expected=$4
  local actual

  actual=$(git ls-remote "$url" "$ref" | awk 'NR == 1 {print $1}')
  if [[ -z "$actual" ]]; then
    printf 'FAIL  %-24s ref unavailable: %s\n' "$label" "$ref"
    failures=$((failures + 1))
  elif [[ "$actual" != "$expected" ]]; then
    printf 'WARN  %-24s branch moved: expected %s, current %s\n' \
      "$label" "$expected" "$actual"
  else
    printf 'PASS  %-24s %s\n' "$label" "$actual"
  fi
}

check_ref \
  'Lineage manifest' \
  'https://github.com/LineageOS/android.git' \
  'refs/heads/lineage-16.0' \
  'e56f933c20b428410d1c53f41e6c0986880cb362'
check_ref \
  'c5lte device' \
  'https://github.com/vm03/android_device_samsung_c5lte.git' \
  'refs/heads/lineage-16.0' \
  '5a5ee3ae766dfda573f6512223fa724279bafdab'
check_ref \
  'c5lte kernel' \
  'https://github.com/vm03/android_kernel_samsung_c5lte.git' \
  'refs/heads/lineage-16.0' \
  '71b335bed497bbe8b9754c9c986801a66212ce4f'
check_ref \
  'c5lte vendor' \
  'https://github.com/vm03/proprietary_vendor_samsung_c5lte.git' \
  'refs/heads/lineage-16.0' \
  'ada0ddb036c256f43f32593ecf7a2593b7a8298c'
check_ref \
  'Samsung hardware' \
  'https://github.com/LineageOS/android_hardware_samsung.git' \
  'refs/heads/lineage-16.0' \
  '871d3f4777603ff6c33a021fc38c9b2265c54f83'

printf '\nResult: %d unavailable source(s)\n' "$failures"
(( failures == 0 ))
