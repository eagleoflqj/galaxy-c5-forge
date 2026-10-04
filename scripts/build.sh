#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir

usage() {
  cat <<'EOF'
Usage: ./scripts/build.sh --workdir /absolute/android-work --target recovery|rom [--jobs N]

Builds the selected target in the pinned LineageOS 16 container. It does not
flash, boot, or otherwise communicate with a device.
EOF
}

workdir=
target=
jobs=$(getconf _NPROCESSORS_ONLN)

while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
      shift 2
      ;;
    --target)
      target=${2:-}
      shift 2
      ;;
    --jobs)
      jobs=${2:-}
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
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "--jobs must be a positive integer" >&2
  exit 2
fi

case "$target" in
  recovery)
    build_goal=recoveryimage
    ;;
  rom)
    build_goal=bacon
    ;;
  *)
    echo "--target must be recovery or rom" >&2
    exit 2
    ;;
esac

mkdir -p "${project_dir}/logs"
log_file="${project_dir}/logs/build-${target}-$(date -u +%Y%m%dT%H%M%SZ).log"
echo "Writing build log to $log_file"

"${project_dir}/scripts/lineage-env.sh" run "$workdir" \
  bash -lc "set -o pipefail; source build/envsetup.sh; lunch lineage_c5lte-userdebug; mka -j${jobs} ${build_goal}" \
  2>&1 | tee "$log_file"

product_out="${workdir}/out/target/product/c5lte"
if [[ "$target" == recovery ]]; then
  artifact="${product_out}/recovery.img"
  if [[ ! -f "$artifact" ]]; then
    echo "Build completed without expected artifact: $artifact" >&2
    exit 1
  fi
  stat --printf='artifact=%n size=%s bytes\n' "$artifact"
  sha256sum "$artifact"
else
  shopt -s nullglob
  artifacts=("${product_out}"/lineage-*.zip)
  shopt -u nullglob
  if (( ${#artifacts[@]} == 0 )); then
    echo "Build completed without a LineageOS ZIP under: $product_out" >&2
    exit 1
  fi

  artifact=${artifacts[0]}
  for candidate in "${artifacts[@]:1}"; do
    if [[ "$candidate" -nt "$artifact" ]]; then
      artifact=$candidate
    fi
  done

  stat --printf='artifact=%n size=%s bytes\n' "$artifact"
  sha256sum "$artifact"
  unzip -t "$artifact" >/dev/null
  zip_entries=$(unzip -Z1 "$artifact")
  for required_entry in \
    META-INF/com/android/metadata \
    META-INF/com/google/android/updater-script \
    boot.img \
    system.new.dat.br \
    system.transfer.list; do
    if ! grep -Fqx "$required_entry" <<<"$zip_entries"; then
      echo "ROM ZIP lacks required entry: $required_entry" >&2
      exit 1
    fi
  done
  metadata=$(unzip -p "$artifact" META-INF/com/android/metadata)
  if ! grep -Fqx 'pre-device=c5lte' <<<"$metadata"; then
    echo "ROM metadata does not identify pre-device=c5lte" >&2
    exit 1
  fi
  echo "ROM ZIP integrity and c5lte metadata checks passed"
fi
