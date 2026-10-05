#!/usr/bin/env bash
set -euo pipefail

readonly manifest_url='https://github.com/LineageOS/android.git'
readonly manifest_commit='e56f933c20b428410d1c53f41e6c0986880cb362'
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir
readonly local_manifest_source="${project_dir}/manifests/local/c5lte-lineage-16.xml"

usage() {
  cat <<'EOF'
Usage: ./scripts/setup.sh --workdir /absolute/android-work [--jobs N] [--no-sync]

Initializes the pinned LineageOS 16 manifest and installs the fixed c5lte local
manifest. By default it synchronizes sources and writes a resolved manifest to
manifests/locked/lineage-16.0.xml.
EOF
}

workdir=
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
log_file="${project_dir}/logs/repo-sync-lineage-16-$(date -u +%Y%m%dT%H%M%SZ).log"
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

"${project_dir}/scripts/apply-patches.sh" --workdir "$workdir"

repo manifest -r -o "${project_dir}/manifests/locked/lineage-16.0.xml"

echo "Source sync complete: $workdir"
echo "Resolved manifest: ${project_dir}/manifests/locked/lineage-16.0.xml"
