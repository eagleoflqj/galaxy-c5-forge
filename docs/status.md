# Project status

Last updated: 2026-10-03

## Completed

- Read `galaxy-c5-forge-codex-plan.md` and adopted it as the execution constraint.
- Audited the initial host without exposing sensitive environment variables.
- Added an idempotent Ubuntu host-tool installer, host diagnostics, and a pinned Ubuntu 20.04 container definition for the LineageOS 16 candidate baseline.
- Added a container launcher that keeps the Android checkout outside this orchestration repository and keeps ccache local and ignored.
- Documented why Python 2 and legacy ncurses stay in the container and why no host JDK is selected for LineageOS 16.
- Installed and verified all host-side packages requested by `scripts/install-host-tools.sh`.
- The user initialized the local Git repository on the default `master` branch with no remote configured. The root `init` commit contains only the standard Apache License 2.0 text; the project baseline follows in a separate commit.
- Removed fixed-column hard wrapping from all repository Markdown files and recorded the no-hard-wrap preference in the global Codex instructions.

## Evidence

- Host: Ubuntu 26.04 LTS, x86_64, 16 logical CPUs, 62 GiB RAM, about 771 GiB free on the workspace filesystem.
- Installed host versions include Repo launcher 2.54 (Ubuntu package 2.58-4), Git LFS 3.7.1, ccache 4.12.3, Android platform tools 34.0.5, Heimdall 2.1.0, LZ4 1.10.0, and ShellCheck 0.11.0.
- `scripts/doctor.sh` passes every host command check. Python 2 remains intentionally isolated to the build container, and LineageOS 16 will use its source-provided JDK.
- `apt-cache` confirms the host packages named by `scripts/install-host-tools.sh` are available on this Ubuntu release.
- All repository shell scripts pass `bash -n` and ShellCheck.
- The first container build exposed that `repo` is unavailable from the enabled Ubuntu 20.04 package sources. The Dockerfile now obtains Repo v2.54 from Google's official `git-repo` repository and verifies its complete commit ID before installation.
- The user reports that the current account now has direct Docker access, so container and diagnostic commands should run without `sudo`.
- The user reported an initial diagnostic result of zero failures and zero warnings. The diagnostic has since been strengthened to require the pinned image and run an in-container tool smoke test. The user reran it and reported zero failures and zero warnings, completing the tool-environment setup.
- Completed the P0 public-source inventory across every visible device and kernel branch, tags, commit history, public forks, upstream Lineage projects, Samsung's official update record, and the two candidate community/vendor portals. Exact revisions and access failures are in `docs/inventory.md`.
- Added a source-evidence hardware and partition fact table in `docs/hardware.md`; all values that still require firmware/device proof are explicitly marked as candidates.
- Selected pinned LineageOS 16.0 plus standard Lineage recovery as the minimum build baseline. The rationale, alternatives, and unresolved mixed-firmware risks are recorded in `docs/baseline-decision.md`.
- Added a SHA-pinned local manifest and executable source-audit, setup, and recovery/ROM build entry points. None of these scripts flash or modify a device.
- Synchronized a clean LineageOS 16 Android worktree at `/mnt/storage/android/galaxy-c5-lineage-16`. `repo status` reports a clean tree and the five explicitly selected projects match their pinned SHAs.
- Exported the complete 746-line resolved manifest to `manifests/locked/lineage-16.0.xml`; after removing whitespace-only padding lines, its SHA-256 is `97f3b77948bf7e86cf0550952e685669fdfc58ae7483e28271edc9aa94a3ae63`.
- Verified in the pinned container that `lunch lineage_c5lte-userdebug` resolves to Android 9, arm64 primary/ARM secondary, device `c5lte`, and product output `/android/out/target/product/c5lte`.
- Built `recoveryimage` successfully in 4 minutes 35 seconds. The 16,236,560 byte artifact has SHA-256 `d0d6b18ff371bca275a354c07b1f9528fe0259dc557e9439511002e0535d78f3`. Read-only header, DT container, ramdisk, recovery/ADB ELF, fstab, property, and tail-marker checks passed; evidence is in `docs/build.md`.
- Added `scripts/inspect-recovery.sh` to repeat image-magic, size, expected-hash, Samsung tail-marker, and `abootimg` checks without modifying the image.
- Resolved a late full-build failure caused by an unmaterialized Git LFS pointer for the arm64 WebView prebuilt. `scripts/setup.sh` now requires Git LFS and explicitly pulls that object after every source sync.
- Built the complete LineageOS `bacon` target successfully and then repeated an incremental build with the new automated artifact checks. The current 618,185,673-byte ROM ZIP has SHA-256 `49d2bd7bc26f142716fc38791bc1bcab92f4369eb09349b561e80926ee0c2568`; full ZIP CRC, required-entry, and `pre-device=c5lte` checks pass. Exact logs, intermediate artifact hashes, and the non-reproducibility observation are in `docs/build.md`.
- `scripts/build.sh --target rom` now verifies the latest Lineage ZIP rather than treating Ninja's exit status alone as sufficient evidence.

## Current blockers

- No SM-C5000 stock firmware, stock boot/recovery, PIT, or specified test device is available. This blocks firmware provenance, actual partition/boot format confirmation, and hardware verification, but not source sync/build.
- The public proprietary vendor tree has no clear redistribution license and is not demonstrably extracted from one matching stock firmware release.
- The baseline has produced recovery and ROM artifacts, but neither has been tested on hardware and no hardware claim has passed testing.
- The built image header uses mkbootimg default ramdisk/tags addresses rather than the unused offset variables declared by the device tree. A matching stock boot/recovery header is required before choosing a corrective patch or proposing hardware testing.
- No device data has been collected or modified. Flashing, unlocking, wiping, repartitioning, and device writes remain unauthorized.

## Active work and observed failures

- The first source sync attempt used 16 concurrent jobs and downloaded about 17 GiB before `android.googlesource.com` returned `RESOURCE_EXHAUSTED` while fetching `platform/system/gatekeeper`. This is a transient service quota failure, not a manifest resolution failure. The setup script now defaults to four jobs, retries each fetch up to three times, explicitly skips tags, and reuses the partial tree. A retry was stopped cleanly after it exposed that omitting `--no-tags` caused existing projects to fetch unnecessary tag history; no downloaded project was removed.
- The interrupted attempt also left stale Git lock files and one half-created `system/hardware/interfaces` gitdir. After confirming no Git process was active, only the stale locks were removed; the one invalid gitdir/object pair was moved to `/tmp/galaxy-c5-invalid.rWUu6G` and recreated successfully by a one-project sync. A final full sync and `repo status` then passed.
- The first full ROM build reached 99% after 1 hour 2 minutes 51 seconds, then failed because `external/chromium-webview/prebuilt/arm64/webview.apk` was a 134-byte Git LFS pointer. Explicitly pulling the declared 243,509,357-byte object produced SHA-256 `446ead71292578830d99e690fd8dded73bc63d85df80f0cfba37762ccb330326`, matching the pointer, and `unzip -t` passed. The incremental retry and a validation rerun both completed successfully.

## Next executable action

Obtain and hash a matching SM-C5000 stock firmware package, then compare its boot/recovery headers, DT packaging, ramdisk and partition metadata with the built candidate. Confirm whether the exact `C5000ZHU1CRJ5` bootloader assertion is appropriate for the intended test device. Do not flash the current recovery or ROM artifacts.
