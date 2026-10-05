#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir
readonly expected_image_size=3196059648
readonly expected_image_sha256=1a113c05150d765a5e8142eac2b3d4bb29b2eecbd776fd37092a6338678f50b6

usage() {
  cat <<'EOF'
Usage: ./scripts/import-stock-gnss.sh --workdir /absolute/android-work --system-image /absolute/system.raw.ext4

Imports only the hash-pinned CSA1 GNSS files from the matching raw stock
system image into the Android worktree's proprietary vendor directory. It
also rewrites three conflicting library names in the imported ELF dependency
graph without changing ELF segment sizes. It does not communicate with or
modify a phone.
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
for tool in debugfs file readelf sha256sum install python3 stat; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required host tool is missing: $tool" >&2
    exit 2
  fi
done

actual_size=$(stat --printf='%s' "$system_image")
actual_sha256=$(sha256sum "$system_image" | awk '{print $1}')
if [[ "$actual_size" != "$expected_image_size" || "$actual_sha256" != "$expected_image_sha256" ]]; then
  echo "Stock system image does not match C5000ZCU1CSA1 CHC" >&2
  echo "Expected size/SHA-256: $expected_image_size $expected_image_sha256" >&2
  echo "Actual size/SHA-256:   $actual_size $actual_sha256" >&2
  exit 1
fi

"$project_dir/scripts/apply-patches.sh" --workdir "$workdir"

device_dir="$workdir/device/samsung/c5lte"
vendor_dir="$workdir/vendor/samsung/c5lte"
if [[ ! -d "$device_dir/.git" || ! -d "$vendor_dir/.git" ]]; then
  echo "Expected c5lte device and vendor Git projects are missing" >&2
  exit 2
fi
if ! grep -Fqx 'vendor/bin/hw/vendor.samsung.hardware.gnss@1.0-service|a244085627cba7ad4bfb30c7e0e7ea246772fdcd' "$device_dir/proprietary-files.txt"; then
  echo "CSA1 GNSS proprietary manifest is not applied" >&2
  exit 1
fi

stage_dir=$(mktemp -d /tmp/c5-csa1-gnss.XXXXXX)
trap 'rm -rf -- "$stage_dir"' EXIT

files=(
  '/vendor/bin/hw/vendor.samsung.hardware.gnss@1.0-service|vendor/bin/hw/vendor.samsung.hardware.gnss@1.0-service|74ab191fcfbf76884de2fa2f0c0d59838c7f787b27b7174806951d888debb485'
  '/vendor/bin/loc_launcher|vendor/bin/loc_launcher|332beddd8afcce9c3a476c72be04e97d5a2550eccac003332454c21739900fe9'
  '/vendor/bin/lowi-server|vendor/bin/lowi-server|b88e5d26e7b6ff44629d4f5c2bc92290b6704b333efcd847442f58ce52a80af8'
  '/vendor/bin/xtra-daemon|vendor/bin/xtra-daemon|8e17f07eb439aa363843bc858f1e5468422dfa06df415aee408b6e9bdf139792'
  '/vendor/bin/xtwifi-client|vendor/bin/xtwifi-client|538052d8b47df319f5f9f7b1912440798de69b72fee5ef593440ecd3a382f6db'
  '/vendor/bin/xtwifi-inet-agent|vendor/bin/xtwifi-inet-agent|1760e9e53db745ec7f5c19a9f85a493a4e085d3743ae04f7e8d369e6977dbf8c'
  '/vendor/etc/cacert_location.pem|vendor/etc/cacert_location.pem|ebc88e16d5a3a3bac3d3a2c79af7b8b61a4be7bfae58cc3c0d6966c235b1eee1'
  '/vendor/etc/flp.conf|vendor/etc/flp.conf|c2d78267ebea2c0ec5aa79ef66ad758dde97c41a8819690598a777f11d612f53'
  '/vendor/etc/gps.conf|vendor/etc/gps.conf|81c8295e78d16a45b14aa9ea624b7bcb0a849499f8c847a5671100dce551f3d1'
  '/vendor/etc/izat.conf|vendor/etc/izat.conf|71e10c672feb608aeacd3c6e8c77134a61750f45dd205a9ce94b3bd3e9772e80'
  '/vendor/etc/lowi.conf|vendor/etc/lowi.conf|67ce3b688f79dd89f1d60553871eb63e2447a1b246d52d4b0fa4877d939381c6'
  '/vendor/etc/sap.conf|vendor/etc/sap.conf|113bf0c631f8f88bb0bf6bc81ae6d910dd831f63d9a6db798ce0377d305e293e'
  '/vendor/etc/xtra_root_cert.pem|vendor/etc/xtra_root_cert.pem|1ce802c2d316598b7860ac06087429b32ef3ef1820c827b28b619781b30842bf'
  '/vendor/lib/hw/android.hardware.gnss@1.0-impl-qti.so|vendor/lib/hw/android.hardware.gnss@1.0-impl-qti.so|9fe81c1d5eb9a481b5a7dddce63726503712676f523091a7dbc7c5ad7ac1e85a'
  '/vendor/lib/hw/vendor.qti.gnss@1.0-impl.so|vendor/lib/hw/vendor.qti.gnss@1.0-impl.so|b80225aba0e9a8d8b47879ffef5c39765b0d1e3294967946aa903808704becd9'
  '/vendor/lib/hw/vendor.samsung.hardware.gnss@1.0-impl.so|vendor/lib/hw/vendor.samsung.hardware.gnss@1.0-impl.so|074c93925b70a8c92b60b1be32cf9ea4e21a97bedb0749691b3d885594094ac7'
  '/lib/libfloatingfeature.so|vendor/lib/libfloatingfeature.so|b8e20be9f815cb1990433077ab99d9dc9bff2423e553f8bd6270104b5e27f070'
  '/lib/libsecnativefeature.so|vendor/lib/libsecgnssfeature.so|ea82df289f1612d54604a59f3d1db7cc23e7a8add0ca205472293ed8930b961e'
  '/vendor/lib/libgps.utils.so|vendor/lib/libgps.csa1.so|3aaf57819865d9d3781ee16275108aeb3f7193bcd183bd9248c31bcb3c51f96d'
  '/vendor/lib/libloc_core.so|vendor/lib/libloc_c5.so|068d4e67bbf2c3b5afe77c9d2d63c3f8b0a154510f143f7c042fd47783bc9dba'
  '/vendor/lib/libDRPlugin.so|vendor/lib/libDRPlugin.so|36cd40595e006238827d0e1ebb5290b2f321a10a57b2903fa7adcd1e79094d64'
  '/vendor/lib/libdataitems.so|vendor/lib/libdataitems.so|3432325fce7ed221753e495be3878ee3bf81a8a78806e114612d390ae4eeb367'
  '/vendor/lib/libdrplugin_client.so|vendor/lib/libdrplugin_client.so|92aa0d39ecc7beebcd3e05dc92fe91e94f3fedf3ca066513587f09687aa379b2'
  '/vendor/lib/libevent_observer.so|vendor/lib/libevent_observer.so|e86f57ac8336faaf6a463864c572b20c39d08b49b89d27d1a0f3e1154cb260ce'
  '/vendor/lib/libflp.so|vendor/lib/libflp.so|35a873d9137b63aadb1e71b3fbd06ef6b38177257a5b8bac69d778519f693954'
  '/vendor/lib/libgdtap.so|vendor/lib/libgdtap.so|efd0ed3a6114f392c94caf46ca5be6397dceb930a6b4070c1953f66791ccd742'
  '/vendor/lib/libgeofence.so|vendor/lib/libgeofence.so|58f3e95b9a9ed32416cfc3c54ff582b4d9cf911af83fd9b92fc96d4e022dd4a0'
  '/vendor/lib/libgnss.so|vendor/lib/libgnss.so|a86756cf9daf01aea366d592c8ac2b18d5c7510e7995948e835e779faef67f51'
  '/vendor/lib/libgnsspps.so|vendor/lib/libgnsspps.so|5388899872bf869b8f15d98ec67fdff2c69d82f6b713f19f2eb51f72d790bf41'
  '/vendor/lib/libizat_client_api.so|vendor/lib/libizat_client_api.so|eba4653897a671674bd2b49e2c4b814d3f1f7007cfe00758315487c4b9169703'
  '/vendor/lib/libizat_core.so|vendor/lib/libizat_core.so|908712d9ae82bca71f8efc8f063bc594b1a904a357467e4aa5262ca206d7a64e'
  '/vendor/lib/liblbs_core.so|vendor/lib/liblbs_core.so|d505da568bce714f046d3f5b01203384017c738e6b03cc101e0ce911f76760ba'
  '/vendor/lib/libloc_api_v02.so|vendor/lib/libloc_api_v02.so|dfeb8da76e6a73d1fc8ed6c4af02657733af1fccc4864e701d2a0b782fb6a667'
  '/vendor/lib/libloc_pla.so|vendor/lib/libloc_pla.so|d120990dc06cc050692736db5a0c090b8fc924ca2d6ccf7f5fe68c20e938dec3'
  '/vendor/lib/libloc_stub.so|vendor/lib/libloc_stub.so|8e679d5116c3a4bdaa45a85633f99e617440623053add7bfe7d569444b1468a1'
  '/vendor/lib/liblocation_api.so|vendor/lib/liblocation_api.so|e3a2fa976ba1b34d4f919a94161ee1038ef089be7226646935c90310637430a1'
  '/vendor/lib/liblocationservice.so|vendor/lib/liblocationservice.so|414f35fbc2b0f08196400d8b85cd8b11a82c9eba735811c1a63e48bb8546a2b3'
  '/vendor/lib/liblocationservice_glue.so|vendor/lib/liblocationservice_glue.so|3f3a54df88bd2ebec61faf8561b36938389cd4e944e5f7cf21994eb3b433ea76'
  '/vendor/lib/liblowi_client.so|vendor/lib/liblowi_client.so|84a5e26d54a9de6d00f9124742cca44bac48a5e6bf78f41b4787e3a90ddca103'
  '/vendor/lib/liblowi_wifihal.so|vendor/lib/liblowi_wifihal.so|8fefd350641549f6ffbb64a98d3bfc66a39ad4d26c34de59337367f9e2b2e96e'
  '/vendor/lib/libulp2.so|vendor/lib/libulp2.so|94fcfd014ecef313a7e3079395971324166de366a440103c2ec0b63c510d713e'
  '/vendor/lib/libxtadapter.so|vendor/lib/libxtadapter.so|d3c3e782eb4fb200091eda13f42730e7bbe0b9673ff1dc38b642ba2a08b9c8f0'
  '/vendor/lib/libxtwifi_ulp_adaptor.so|vendor/lib/libxtwifi_ulp_adaptor.so|53aea74f034d651ab7bffd49feedd5fef01dfb94cbd725bfc7cecef7ce05fe8a'
  '/vendor/lib/vendor.qti.gnss@1.0_vendor.so|vendor/lib/vendor.qti.gnss@1.0_vendor.so|8cf49f254ba769d795a2a20aa4235b9fde3c422ee8bea0149d56efc86b8d1a4d'
  '/vendor/lib/vendor.samsung.hardware.gnss@1.0.so|vendor/lib/vendor.samsung.hardware.gnss@1.0.so|bcf2847a9ddd1e7869d374cfcb06a03f26cbfecfc9dd74d49a144aa8f8817e7f'
)

patched_files=(
  'vendor/bin/loc_launcher|28e76ef8fba34d725b2c10fdf018eba12bdc10ded43ae2b10e0e370be15e969f'
  'vendor/bin/xtra-daemon|9e2a931eeef32b28a1d970bcaeb9c36f9a145081d23edc05f307f9934154c9c9'
  'vendor/bin/xtwifi-client|7328c92cb85c00da3cfb322aa0643551a654d92c8fd83f3e1d45737af52b4ad0'
  'vendor/lib/hw/android.hardware.gnss@1.0-impl-qti.so|6648e825957be2ec5e580985822527b9861b3b178728cbd663ddf3adb0ed0f6d'
  'vendor/lib/hw/vendor.qti.gnss@1.0-impl.so|c629a58ee3d725a2d181e21e4c5293abc343149c7c1f0a278db0f25fe216f705'
  'vendor/lib/libDRPlugin.so|5ab99544f12abb48328239839283a0cb711c5420a197b32d7bf1f12a593fb0bf'
  'vendor/lib/libdataitems.so|dc1d62daa0e9404c9276bf38a97114ed77e1a8b8abe4b355446818c9cf5a7213'
  'vendor/lib/libdrplugin_client.so|ab1a36ea7ae256b98011a135bc95edafac28e1da50c28e49479f8968fbf5e6d4'
  'vendor/lib/libevent_observer.so|bc7ff3da9d9474ab808aa0e069459ddfe83de7390ed8a03a0cc2737869f4d9cf'
  'vendor/lib/libflp.so|18f60ac941da1f5939521810f5947a542ab1adae86eae6c6afae6e942689bd5c'
  'vendor/lib/libgdtap.so|dbf25e19575dd8fab9cdefb824cb1daa627e720ceca92a69e85d852da54bd489'
  'vendor/lib/libgeofence.so|e1603e8457123c2fab9857a641a8d25b40960c276c74d26c98ece49edfaf05ee'
  'vendor/lib/libgnss.so|47c4c9dea29e50677aaea74bea88bf45d2369a3a6dc1440cc68bf7ec0d264220'
  'vendor/lib/libgnsspps.so|3da023e33e950ee63edab530e71e00592dd4cb253cb4850a7910590a010e4550'
  'vendor/lib/libgps.csa1.so|523a90362bff24da15b6e3cc79b6e449a1670542675e9d8b0d928faf09984660'
  'vendor/lib/libizat_client_api.so|394d74b789d6a988fd62540a3aa5d61a8d7f374d4bf442fcf994b6c58bc64298'
  'vendor/lib/libizat_core.so|45fe33c9f282f98b3dac370c2871f12609a58b5326835b9e32d34aed5eb401eb'
  'vendor/lib/liblbs_core.so|dd66e9acdcd92eca084b5e585cae22a97a34399f4a79630d6d8ff33b6e7c4ef8'
  'vendor/lib/libloc_api_v02.so|15c318fe1aa6019bca4fe31529cc6676078194ad8463768d169ba555aca7c245'
  'vendor/lib/libloc_c5.so|c75313590077f97b6dd8399bac4bed481eaf9e972a7917e579cb11334847f780'
  'vendor/lib/liblocation_api.so|830ec23a7c69fc09a5167af69b09f679a2ecd5eea5bcb4c3645ef526b4da7d9d'
  'vendor/lib/liblocationservice.so|b8823a2a8e859f1317358f8a9342abe9107a76ef0e2ef86478498e1d3def7ace'
  'vendor/lib/liblocationservice_glue.so|8924e8930bb13816d2cc100bcdc6b6617785627bec1c8f9cd43c0fd2e63de95b'
  'vendor/lib/libsecgnssfeature.so|4c355465500c81b44265a4ed05ce84dfb6a8b480c314428f90e39fad4917f3f3'
  'vendor/lib/libulp2.so|770bdb8101141936e31c48817673e621d34714406708486c7ac19f309d08b1b8'
  'vendor/lib/libxtadapter.so|38996989e16c7fcfabc72b86d5fecaf07dcdb869d544fc3ca0a2a9bfe0f4227d'
)

for record in "${files[@]}"; do
  IFS='|' read -r source_path target_path expected_sha256 <<< "$record"
  staged="$stage_dir/$target_path"
  install -d -m 700 "$(dirname "$staged")"
  debugfs -R "dump -p $source_path $staged" "$system_image" >/dev/null 2>&1
  actual_file_sha256=$(sha256sum "$staged" | awk '{print $1}')
  if [[ "$actual_file_sha256" != "$expected_sha256" ]]; then
    echo "Hash mismatch for $source_path" >&2
    echo "Expected SHA-256: $expected_sha256" >&2
    echo "Actual SHA-256:   $actual_file_sha256" >&2
    exit 1
  fi
done

rewrite_sec=0
rewrite_gps=0
rewrite_loc=0
for record in "${files[@]}"; do
  IFS='|' read -r source_path target_path expected_sha256 <<< "$record"
  staged="$stage_dir/$target_path"
  if ! file "$staged" | grep -q 'ELF'; then
    continue
  fi
  dynamic=$(readelf -d "$staged" 2>/dev/null || true)
  replacements=()
  if grep -Fq '[libsecnativefeature.so]' <<< "$dynamic"; then
    replacements+=(libsecnativefeature.so=libsecgnssfeature.so)
    rewrite_sec=$((rewrite_sec + 1))
  fi
  if grep -Fq '[libgps.utils.so]' <<< "$dynamic"; then
    replacements+=(libgps.utils.so=libgps.csa1.so)
    rewrite_gps=$((rewrite_gps + 1))
  fi
  if grep -Fq '[libloc_core.so]' <<< "$dynamic"; then
    replacements+=(libloc_core.so=libloc_c5.so)
    rewrite_loc=$((rewrite_loc + 1))
  fi
  if (( ${#replacements[@]} > 0 )); then
    "$project_dir/scripts/patch-elf-dynstr.py" "$staged" "${replacements[@]}"
  fi
done

if (( rewrite_sec != 7 || rewrite_gps != 25 || rewrite_loc != 19 )); then
  echo "Unexpected CSA1 ELF rewrite counts: sec=$rewrite_sec gps=$rewrite_gps loc=$rewrite_loc" >&2
  exit 1
fi

for record in "${patched_files[@]}"; do
  IFS='|' read -r target_path expected_sha256 <<< "$record"
  staged="$stage_dir/$target_path"
  actual_file_sha256=$(sha256sum "$staged" | awk '{print $1}')
  if [[ "$actual_file_sha256" != "$expected_sha256" ]]; then
    echo "Post-rewrite hash mismatch for $target_path" >&2
    echo "Expected SHA-256: $expected_sha256" >&2
    echo "Actual SHA-256:   $actual_file_sha256" >&2
    exit 1
  fi
done

declare -A output_hashes=()
for record in "${files[@]}"; do
  IFS='|' read -r source_path target_path expected_sha256 <<< "$record"
  output_hashes["$target_path"]=$expected_sha256
done
for record in "${patched_files[@]}"; do
  IFS='|' read -r target_path expected_sha256 <<< "$record"
  output_hashes["$target_path"]=$expected_sha256
done

for record in "${files[@]}"; do
  IFS='|' read -r source_path target_path expected_sha256 <<< "$record"
  target="$vendor_dir/proprietary/$target_path"
  install -d -m 755 "$(dirname "$target")"
  install -m 0644 "$stage_dir/$target_path" "$target"
  actual_file_sha256=$(sha256sum "$target" | awk '{print $1}')
  if [[ "$actual_file_sha256" != "${output_hashes[$target_path]}" ]]; then
    echo "Post-install hash mismatch for $target" >&2
    exit 1
  fi
done

"$device_dir/setup-makefiles.sh"

printf 'Imported and verified %s CSA1 GNSS files from %s\n' "${#files[@]}" "$system_image"
printf 'Rewrote and hash-verified %s CSA1 ELF files without changing section sizes\n' "${#patched_files[@]}"
printf 'Regenerated proprietary makefiles under %s\n' "$vendor_dir"
