# Build environment

## Layout

The Ubuntu 26.04 host is used for source management, firmware inspection, image tools, and USB clients. LineageOS 16 builds run in the pinned Ubuntu 20.04 container in `containers/lineage-16/`. This keeps Python 2 and legacy ncurses libraries out of the host and avoids treating the newer host distribution as a validated Android 9 build platform.

LineageOS 16 carries its expected OpenJDK 9 prebuilt in the source checkout, so the host installer deliberately does not choose a system Java version.

## Install and verify host tools

```bash
sudo ./scripts/install-host-tools.sh
./scripts/doctor.sh
```

If Docker is intentionally root-only, use `sudo ./scripts/doctor.sh` for the final Docker-access check. When `docker info` works as the current user, do not use `sudo` for either script.

The installer is idempotent and uses only Ubuntu packages. It installs build utilities, Repo, Git LFS, ADB, Fastboot, Heimdall, boot/device-tree utilities, Android sparse-image tools, filesystem/archive tools, and shell validation. The diagnostic also requires the pinned build image to exist and smoke-tests Repo, Python 2/3, ccache, legacy ncurses, and core Android image tools inside that image.

`scripts/setup.sh` explicitly runs `git lfs pull` for the arm64 Chromium WebView prebuilt after Repo synchronization. Without that step, a host with LFS smudge disabled leaves a small pointer file in place of the APK and the ROM build fails only near the end when `signapk` tries to open it.

The udev package supplies Android USB rules. A replug or udev reload may be needed after installation. No device-writing command is run by the installer or the diagnostics.

## Build and enter the LineageOS 16 container

Docker access is security-sensitive: membership in the `docker` group normally confers root-equivalent access. This project does not add users to that group. Use the system's approved Docker access policy. If Docker is intentionally kept root-only, the launcher may be called with `sudo`; it preserves `SUDO_UID` and `SUDO_GID` so files created in the mounted worktree remain owned by the invoking user.

```bash
./scripts/lineage-env.sh build-image
./scripts/lineage-env.sh run /absolute/path/to/android-work
```

For a root-only Docker daemon, use `sudo` before both commands above. When the current user already has deliberate Docker access, run the commands exactly as shown without `sudo`. Docker-group membership is root-equivalent access.

Inside the container, `/android` is the external Android source worktree, `/forge` is this repository, and `/ccache` is backed by `.cache/ccache`.

The base is the official `ubuntu:20.04` image pinned by index digest. Package versions are not yet snapshotted independently; the final reproducible-build milestone must also lock the resulting container image digest.

Ubuntu 20.04's enabled package sources do not provide the `repo` package in this container. The Dockerfile therefore installs the launcher directly from the official `git-repo` repository, pins tag `v2.54`, and verifies that the checkout resolves to commit `97dc5c1bd9527c2abe2183b16a4b7ef037dc34a7` before installing it.

## Codex execution permissions

The normal `workspace-write` sandbox protects `.git`, blocks the Docker socket in this environment, limits writes outside the repository, and restricts shell network access. Those boundaries are useful for ordinary edits but interrupt Android source synchronization and containerized builds.

For autonomous work on this trusted project, start a project-specific Codex session with Full access (`sandbox_mode = "danger-full-access"` and `approval_policy = "never"`). Do not make that the global default. Because the current user belongs to the `docker` group, granting Codex Docker access is effectively root-equivalent host access. Keep firmware, credentials, signing keys, device backups, and unrelated directories outside the working scope.

A lower-risk alternative is to keep `workspace-write`, enable shell network access, add only the external Android worktree as a writable root, and approve specific out-of-sandbox commands. That mode may still require manual help for Docker and protected `.git` metadata.
