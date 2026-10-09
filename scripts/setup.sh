#!/usr/bin/env bash
set -euo pipefail

readonly manifest_url='https://github.com/LineageOS/android.git'
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir

usage() {
  cat <<'EOF'
Usage: ./scripts/setup.sh --workdir /absolute/android-work [--lineage-version 16.0|17.1] [--jobs N] [--no-sync]

Initializes the selected pinned LineageOS manifest and installs its fixed c5lte
local manifest. By default it synchronizes sources and writes a resolved
manifest under manifests/locked/.
EOF
}

workdir=
lineage_version=16.0
# Android's many independent fetches can trigger googlesource per-client quota
# limits at CPU-count concurrency. Keep the reliable default conservative;
# callers can still opt into a higher value.
jobs=4
do_sync=true

while (( $# > 0 )); do
  case "$1" in
    --workdir)
      workdir=${2:-}
      shift 2
      ;;
    --lineage-version)
      lineage_version=${2:-}
      shift 2
      ;;
    --jobs)
      jobs=${2:-}
      shift 2
      ;;
    --no-sync)
      do_sync=false
      shift
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

if [[ -z "$workdir" || "$workdir" != /* ]]; then
  echo "--workdir must be an absolute path" >&2
  exit 2
fi
case "$lineage_version" in
  16.0)
    manifest_commit='e56f933c20b428410d1c53f41e6c0986880cb362'
    local_manifest_source="${project_dir}/manifests/local/c5lte-lineage-16.xml"
    locked_manifest="${project_dir}/manifests/locked/lineage-16.0.xml"
    log_version=16
    ;;
  17.1)
    manifest_commit='c7b3d4b65b79e2e0d6a891547ad138e1fcdafb56'
    local_manifest_source="${project_dir}/manifests/local/c5lte-lineage-17.1.xml"
    locked_manifest="${project_dir}/manifests/locked/lineage-17.1.xml"
    log_version=17.1
    ;;
  *)
    echo "--lineage-version must be 16.0 or 17.1" >&2
    exit 2
    ;;
esac
readonly manifest_commit local_manifest_source locked_manifest log_version
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "--jobs must be a positive integer" >&2
  exit 2
fi
if [[ ! -f "$local_manifest_source" ]]; then
  echo "Missing local manifest: $local_manifest_source" >&2
  exit 2
fi
if ! command -v repo >/dev/null 2>&1; then
  echo "repo is not installed; run scripts/install-host-tools.sh" >&2
  exit 2
fi
if ! git lfs version >/dev/null 2>&1; then
  echo "git-lfs is not installed; run scripts/install-host-tools.sh" >&2
  exit 2
fi

mkdir -p "$workdir"
available_kib=$(df -Pk "$workdir" | awk 'NR == 2 {print $4}')
required_kib=$((200 * 1024 * 1024))
if (( available_kib < required_kib )); then
  echo "At least 200 GiB free is required; found $((available_kib / 1024 / 1024)) GiB" >&2
  exit 1
fi

cd "$workdir"
repo init \
  --manifest-url "$manifest_url" \
  --manifest-branch "$manifest_commit" \
  --depth=1 \
  --no-clone-bundle \
  --no-tags

mkdir -p .repo/local_manifests
install -m 0644 "$local_manifest_source" .repo/local_manifests/c5lte.xml

if [[ "$do_sync" == false ]]; then
  echo "Initialized without syncing: $workdir"
  exit 0
fi

mkdir -p "${project_dir}/logs" "${project_dir}/manifests/locked"
log_file="${project_dir}/logs/repo-sync-lineage-${log_version}-$(date -u +%Y%m%dT%H%M%SZ).log"
echo "Writing sync log to $log_file"
repo sync \
  --current-branch \
  --jobs="$jobs" \
  --fail-fast \
  --no-clone-bundle \
  --no-tags \
  --optimized-fetch \
  --retry-fetches=3 \
  2>&1 | tee "$log_file"

# Repo checks out Git LFS pointer text when the host is configured to skip
# smudge filters. The arm64 WebView prebuilt is required late in `bacon`, so
# fetch it explicitly now and fail during setup instead of after a long build.
echo "Fetching arm64 WebView Git LFS object"
repo forall external/chromium-webview/prebuilt/arm64 -c 'git lfs pull'

"${project_dir}/scripts/apply-patches.sh" --workdir "$workdir" --lineage-version "$lineage_version"

repo manifest -r -o "$locked_manifest"

echo "Source sync complete: $workdir"
echo "Resolved manifest: $locked_manifest"
