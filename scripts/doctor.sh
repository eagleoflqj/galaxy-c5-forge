#!/usr/bin/env bash
set -uo pipefail

failures=0
warnings=0

pass() { printf 'PASS  %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; warnings=$((warnings + 1)); }
fail() { printf 'FAIL  %s\n' "$*"; failures=$((failures + 1)); }

check_command() {
  local command_name=$1
  local description=$2
  if command -v "$command_name" >/dev/null 2>&1; then
    pass "$description: $(command -v "$command_name")"
  else
    fail "$description: command '$command_name' is missing"
  fi
}

# shellcheck disable=SC1091
source /etc/os-release

echo "Galaxy C5 Forge host environment"
echo "OS: ${PRETTY_NAME:-unknown}"
echo "Kernel: $(uname -srmo)"
echo "CPU threads: $(getconf _NPROCESSORS_ONLN)"
free -h | awk 'NR == 2 {printf "Memory: %s total, %s available\n", $2, $7}'
df -hP . | awk 'NR == 2 {printf "Workspace disk: %s available of %s (%s)\n", $4, $2, $6}'
echo

for spec in \
  'git:Git' \
  'git-lfs:Git LFS' \
  'repo:Repo launcher' \
  'python3:Python 3' \
  'gcc:C compiler' \
  'g++:C++ compiler' \
  'make:GNU Make' \
  'ninja:Ninja' \
  'ccache:ccache' \
  'flex:flex' \
  'bison:bison' \
  'gperf:gperf' \
  'rsync:rsync' \
  'zip:zip' \
  'unzip:unzip' \
  'lz4:LZ4' \
  'simg2img:Android sparse image reader' \
  'img2simg:Android sparse image writer' \
  'mkbootimg:Android boot image builder' \
  'dtc:Device Tree Compiler' \
  'adb:Android Debug Bridge' \
  'fastboot:fastboot client' \
  'heimdall:Heimdall client' \
  'shellcheck:ShellCheck'; do
  check_command "${spec%%:*}" "${spec#*:}"
done

echo
if command -v docker >/dev/null 2>&1; then
  if docker info >/dev/null 2>&1; then
    pass "Docker daemon is accessible"
    container_image='galaxy-c5-forge/lineage-16:ubuntu-20.04'
    if image_id=$(docker image inspect --format '{{.Id}}' "$container_image" 2>/dev/null); then
      pass "LineageOS 16 container image exists: ${image_id}"
      if docker run --rm "$container_image" bash -lc '
        set -e
        command -v repo ccache flex bison gperf simg2img img2simg mkbootimg adb >/dev/null
        python -c "import sys; assert sys.version_info[0] == 2"
        python3 -c "import sys; assert sys.version_info[0] == 3"
        ldconfig -p | grep -q "libncurses.so.5"
      ' >/dev/null 2>&1; then
        pass "LineageOS 16 container smoke test"
      else
        fail "LineageOS 16 container smoke test failed"
      fi
    else
      fail "LineageOS 16 container image is missing: $container_image"
    fi
  else
    fail "Docker is installed but the daemon/socket is not accessible"
  fi
else
  fail "Docker client is missing"
fi

if command -v python2 >/dev/null 2>&1; then
  warn "Host Python 2 is present; builds should still use the pinned container"
else
  pass "Host Python 2 intentionally absent (provided by build container)"
fi

if command -v java >/dev/null 2>&1; then
  warn "Host Java is present; LineageOS 16 should use its source prebuilt JDK"
else
  pass "Host Java intentionally not required for the selected container workflow"
fi

echo
printf 'Result: %d failure(s), %d warning(s)\n' "$failures" "$warnings"
(( failures == 0 ))
