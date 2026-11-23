#!/usr/bin/env bash
set -euo pipefail

readonly manifest_url='https://github.com/LineageOS/android.git'
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir

usage() {
  cat <<'EOF'
Usage: ./scripts/setup.sh --workdir /absolute/android-work [--lineage-version 16.0|17.1|18.1] [--jobs N] [--no-sync]

Initializes the selected pinned LineageOS manifest and installs its fixed c5lte
local manifest. Project revisions come from manifests/locked/. By default it
synchronizes sources and saves the observed revisions alongside the sync log.
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
  18.1)
    manifest_commit='75e412bbb78aa0009405f696d0d3775e995e64ca'
    local_manifest_source="${project_dir}/manifests/local/c5lte-lineage-18.1.xml"
    locked_manifest="${project_dir}/manifests/locked/lineage-18.1.xml"
    log_version=18.1
    ;;
  *)
    echo "--lineage-version must be 16.0, 17.1 or 18.1" >&2
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

log_dir="${project_dir}/logs"
repo_flags=()
if [[ "$lineage_version" == 18.1 ]]; then
  root=$(dirname "$(realpath "$workdir")")
  export HOME="$root/home" TMPDIR="$root/tmp" GIT_ALLOW_PROTOCOL=https GIT_LFS_SKIP_SMUDGE=1
  log_dir="$root/logs"
  install -d -m 700 "$HOME" "$TMPDIR"
  repo_flags+=(--repo-rev=v2.54)
fi
cd "$workdir"
repo init "${repo_flags[@]}" \
  --manifest-url "$manifest_url" \
  --manifest-branch "$manifest_commit" \
  --depth=1 \
  --no-clone-bundle \
  --no-tags

if [[ "$lineage_version" == 18.1 && $(git -C .repo/repo rev-parse HEAD) != 97dc5c1bd9527c2abe2183b16a4b7ef037dc34a7 ]]; then
  echo "Unexpected git-repo version for LineageOS 18.1" >&2
  exit 1
fi
mkdir -p .repo/local_manifests
install -m 0644 "$local_manifest_source" .repo/local_manifests/c5lte.xml

python3 - "$locked_manifest" .repo/local_manifests/release-revisions.xml <<'PYTHON'
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

overlay = ET.Element("manifest")
for project in ET.parse(sys.argv[1]).getroot().findall("project"):
    name = project.attrib["name"]
    revision = project.attrib["revision"]
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise SystemExit(f"Unpinned project: {name}")
    attrs = {"name": name, "path": project.get("path", name), "revision": revision}
    if "upstream" in project.attrib:
        attrs["upstream"] = project.attrib["upstream"]
    ET.SubElement(overlay, "extend-project", attrs)
output = Path(sys.argv[2])
temporary = output.with_suffix(".tmp")
ET.ElementTree(overlay).write(temporary, encoding="utf-8", xml_declaration=True)
temporary.replace(output)
PYTHON

if [[ "$do_sync" == false ]]; then
  echo "Initialized without syncing: $workdir"
  exit 0
fi

mkdir -p "$log_dir"
log_file="${log_dir}/repo-sync-lineage-${log_version}-$(date -u +%Y%m%dT%H%M%SZ).log"
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

resolved_manifest="${log_file%.log}.xml"
repo manifest -r -o "$resolved_manifest"

echo "Source sync complete: $workdir"
echo "Resolved manifest: $resolved_manifest"
