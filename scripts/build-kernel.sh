#!/usr/bin/env bash
set -euo pipefail

hash_file=baseline.sha256
if [[ ${1:-} == --lineage-version && ${2:-} == 19.1 ]]; then
    hash_file=lineage-19.1.sha256
    shift 2
fi
if [[ $# != 7 ]]; then
    echo "Usage: $0 [--lineage-version 19.1] SOURCE CLANG ARM64_GCC ARM32_GCC FIRMWARE NEW_OUTPUT JOBS" >&2
    exit 2
fi

repo=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
source_tree=$(realpath -e -- "$1")
clang=$(realpath -e -- "$2")
arm64=$(realpath -e -- "$3")
arm32=$(realpath -e -- "$4")
firmware=$(realpath -e -- "$5")
output=$(realpath -m -- "$6")
jobs=$7
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
parent=$(dirname -- "$output")
case "$(findmnt -no FSTYPE -T "$parent")" in
    tmpfs|ramfs|'') echo "Select a disk-backed output directory" >&2; exit 2 ;;
esac
case "$output/" in /tmp/*) echo "Select a persistent build location" >&2; exit 2 ;; esac
mkdir -- "$output"
mkdir -- "$output/build" "$output/tmp"
cp -- "$repo/kernel/config/c5lte.config" "$output/build/.config"

# The inner shell expands the job count in the quoted build command.
# shellcheck disable=SC2016
bwrap --unshare-all --die-with-parent --new-session --clearenv --tmpfs / \
    --ro-bind /usr /usr --symlink usr/bin /bin --symlink usr/lib /lib --symlink usr/lib64 /lib64 \
    --proc /proc --dev /dev --ro-bind /etc/ld.so.cache /etc/ld.so.cache \
    --ro-bind /etc/alternatives /etc/alternatives --dir /c5-runtime/bin --symlink /usr/bin/python3 /c5-runtime/bin/python \
    --ro-bind "$repo/kernel/config/$hash_file" /kernel.sha256 --ro-bind "$source_tree" /kernel --ro-bind "$repo/kernel/techpack.Kbuild" /kernel/techpack/Kbuild --ro-bind "$repo/kernel/extra" /extra \
    --ro-bind "$firmware" /c5-firmware --ro-bind "$clang" /clang --ro-bind "$arm64" /toolchain --ro-bind "$arm32" /arm32 \
    --bind "$output/build" /build --bind "$output/tmp" /tmp --chdir /kernel \
    --setenv PATH /clang/bin:/toolchain/bin:/arm32/bin:/c5-runtime/bin:/usr/bin:/bin \
    --setenv LANG C --setenv LC_ALL C --setenv TMPDIR /tmp --setenv CCACHE_DISABLE 1 \
    --setenv KBUILD_BUILD_USER forge --setenv KBUILD_BUILD_HOST galaxy-c5-v395-rpm-log-a1 \
    --setenv KBUILD_BUILD_TIMESTAMP 'Sun Oct 25 00:00:00 UTC 2026' --setenv KBUILD_BUILD_VERSION 1 \
    --setenv ARCH arm64 --setenv CROSS_COMPILE aarch64-linux-android- --setenv CROSS_COMPILE_ARM32 arm-linux-androideabi- \
    --setenv CLANG_TRIPLE aarch64-linux-gnu- --setenv CLANG_TRIPLE_ARM32 arm-linux-gnueabi- \
    -- /bin/bash -eu -o pipefail -c '
        sha256sum -c - <<CHECK
04903ea8bab69460db821a00b7aa3fc703bb4ec90ad145fbf2808127bd7462d7  /build/.config
a6158cae3ac9912e520f1c9a68bfcb5a0d426587ff1ae9b8ebd74eddcedc527d  /c5-firmware/tsp_zinitix/zt7538_c5.fw
dc4c0463ce1ac14c71b3fd3f91df0269629932ae02e20cde918eb363c689c0cc  /c5-firmware/abov/abov_ft1604_c5.fw
CHECK
        make O=/build CC=clang olddefconfig
        make O=/build CC=clang -j"$1" KCFLAGS="-idirafter/extra -ferror-limit=0" Image.gz
        (cd /build/arch/arm64/boot && sha256sum -c /kernel.sha256)
    ' bash "$jobs"
