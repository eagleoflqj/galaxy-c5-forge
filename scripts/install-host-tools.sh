#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script as root: sudo ./scripts/install-host-tools.sh" >&2
  exit 2
fi

if [[ ! -r /etc/os-release ]]; then
  echo "Cannot identify the host operating system." >&2
  exit 2
fi

# shellcheck disable=SC1091
source /etc/os-release
if [[ ${ID:-} != ubuntu ]]; then
  echo "Unsupported host: ${PRETTY_NAME:-unknown}. This installer targets Ubuntu." >&2
  exit 2
fi

packages=(
  adb
  aria2
  android-sdk-libsparse-utils
  android-sdk-platform-tools-common
  abootimg
  bc
  binwalk
  bison
  build-essential
  ccache
  cpio
  curl
  device-tree-compiler
  fastboot
  flex
  g++-multilib
  gcc-multilib
  git
  git-lfs
  gnupg
  gperf
  heimdall-flash
  imagemagick
  jq
  libc6-dev-i386
  lib32readline-dev
  lib32z1-dev
  libdw-dev
  libelf-dev
  libncurses-dev
  libsdl1.2-dev
  libssl-dev
  libxml2-utils
  lz4
  lzop
  mkbootimg
  pngcrush
  protobuf-compiler
  python3
  python3-protobuf
  python3-venv
  repo
  rsync
  schedtool
  shellcheck
  squashfs-tools
  u-boot-tools
  unzip
  wget
  xsltproc
  xz-utils
  yq
  zip
  zlib1g-dev
)

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install --no-install-recommends -y "${packages[@]}"
git lfs install --system

echo
echo "Host tools installed. Run ./scripts/doctor.sh to verify them."
echo "LineageOS 16 builds use the pinned container; no host Python 2 or JDK is installed."
