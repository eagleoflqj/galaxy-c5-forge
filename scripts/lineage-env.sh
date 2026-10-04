#!/usr/bin/env bash
set -euo pipefail

readonly image_name='galaxy-c5-forge/lineage-16:ubuntu-20.04'
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly project_dir
readonly dockerfile_dir="${project_dir}/containers/lineage-16"
readonly host_uid=${SUDO_UID:-$(id -u)}
readonly host_gid=${SUDO_GID:-$(id -g)}

usage() {
  cat <<'EOF'
Usage:
  ./scripts/lineage-env.sh build-image
  ./scripts/lineage-env.sh run /absolute/android-work [command ...]

The Android source worktree is mounted at /android and the forge repository at
/forge. The ccache directory is kept under .cache/ccache in this repository.
EOF
}

require_docker() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is not installed" >&2
    exit 2
  fi
  if ! docker info >/dev/null 2>&1; then
    echo "docker is installed but its daemon/socket is not accessible" >&2
    echo "Use sudo for a one-off build, or deliberately grant this user Docker access." >&2
    echo "Membership in the docker group is equivalent to root access." >&2
    exit 2
  fi
}

command_name=${1:-}
case "$command_name" in
  build-image)
    require_docker
    docker build \
      --tag "$image_name" \
      "$dockerfile_dir"
    ;;
  run)
    require_docker
    android_work=${2:-}
    if [[ -z "$android_work" || "$android_work" != /* ]]; then
      echo "Android worktree must be an absolute path." >&2
      usage >&2
      exit 2
    fi
    if [[ ! -d "$android_work" ]]; then
      echo "Android worktree does not exist: $android_work" >&2
      exit 2
    fi
    shift 2
    mkdir -p "${project_dir}/.cache/ccache" "${project_dir}/.cache/container-home"
    if (( EUID == 0 )) && [[ -n ${SUDO_UID:-} ]]; then
      chown "$host_uid:$host_gid" \
        "${project_dir}/.cache/ccache" \
        "${project_dir}/.cache/container-home"
    fi
    if (( $# == 0 )); then
      set -- bash
    fi
    docker_tty=()
    if [[ -t 0 && -t 1 ]]; then
      docker_tty=(-it)
    fi
    docker run --rm "${docker_tty[@]}" \
      --user "${host_uid}:${host_gid}" \
      --env HOME=/tmp/home \
      --env USE_CCACHE=1 \
      --env CCACHE_EXEC=/usr/bin/ccache \
      --env CCACHE_DIR=/ccache \
      --volume "${project_dir}:/forge" \
      --volume "${project_dir}/.cache/ccache:/ccache" \
      --volume "${project_dir}/.cache/container-home:/tmp/home" \
      --volume "${android_work}:/android" \
      --workdir /android \
      "$image_name" "$@"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
