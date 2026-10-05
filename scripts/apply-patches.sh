#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir

usage() {
  cat <<'EOF'
Usage: ./scripts/apply-patches.sh --workdir /absolute/android-work

Applies the tracked c5lte source patches idempotently to the pinned Android
worktree. It fails if a patch matches neither the forward nor reverse state.
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

apply_patch_set() {
  local source_dir=$1
  local patch_dir=$2
  local patch

  if [[ ! -d "$source_dir/.git" ]]; then
    echo "Missing Git project: $source_dir" >&2
    return 1
  fi

  shopt -s nullglob
  local patches=("$patch_dir"/*.patch)
  shopt -u nullglob
  if (( ${#patches[@]} == 0 )); then
    echo "No patches found under: $patch_dir" >&2
    return 1
  fi

  for patch in "${patches[@]}"; do
    if git -C "$source_dir" apply --check "$patch" 2>/dev/null; then
      git -C "$source_dir" apply "$patch"
      printf 'APPLIED %s\n' "${patch#"$project_dir/"}"
    elif git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
      printf 'PRESENT %s\n' "${patch#"$project_dir/"}"
    else
      printf 'CONFLICT %s\n' "${patch#"$project_dir/"}" >&2
      return 1
    fi
  done
}

apply_patch_set \
  "$workdir/device/samsung/c5lte" \
  "$project_dir/patches/device-samsung-c5lte"

apply_patch_set \
  "$workdir/bootable/recovery" \
  "$project_dir/patches/bootable-recovery"
