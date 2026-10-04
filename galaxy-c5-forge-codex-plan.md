# Galaxy C5 Forge: Local Codex Execution Plan

This project builds reproducible unofficial LineageOS support for the Samsung Galaxy C5. The provisional target model is SM-C5000, with candidate device codenames c5ltechn / c5lte; the exact identities must be cross-checked against a physical device, firmware, and source. First boot recovery and obtain ADB, then bring up the complete system and hardware functions, and finally evaluate an upgrade to a modern Android version.

This file is a task specification, not a verified flashing guide. Do not treat any candidate repository, device parameter, or build target in it as a confirmed fact.

## 1. Instructions for Codex

You are the implementer of this project. Perform the investigation, coding, scripting, builds, and documentation directly in the current project directory instead of merely giving advice. Read the existing `AGENTS.md` and repository state first, and preserve the user's existing changes. Use a single agent by default.

You may autonomously read source, research online, download public source, analyze firmware supplied by the user, create device trees, modify code, compile, run host checks, and perform read-only ADB investigation on an explicitly designated test device. Every download and modification must be traceable. Explain the specific need before changing host configuration or installing system-level dependencies.

The user currently authorizes software-engineering work. Flashing, unlocking, factory resets, partition erasure, and device-data modification are not included in the default authorization. Before any such operation, prepare the exact image, hash, target device, target partition, backups, and recovery plan, then obtain authorization for that operation. Do not ask again for an operation already covered by an active authorization.

When no device or firmware is temporarily available, continue with investigation, project structure, checks, and scripts that do not depend on them. Do not fabricate device parameters, blobs, successful builds, or hardware-test results. Do not create a superficially complete but unbuildable template and declare the project complete.

At the end of every session, update `docs/status.md` with what was completed, where the evidence is, current blockers, and the next executable action. Preserve complete logs for long builds. Every fix must correspond to an observed error or a specific interface difference; do not apply unsupported batches of hacks.

## 2. Scope and milestones

| Phase | Deliverables | Pass condition |
| --- | --- | --- |
| P0 Investigation | Environment report, source inventory, hardware/partition fact table, baseline selection | Every critical parameter has evidence and every gap is explicit |
| P1 Stock analysis | Repeatable firmware parsing, blob inventory, boot parameters, kernel configuration | Identical input produces identical analysis and the original input remains unchanged |
| P2 Recovery | Device/kernel configuration, recovery image, recovery instructions | Recovery boots on the designated device, ADB works, and logs are retained |
| P3 System boot | Unofficial LineageOS installation package and boot image | The device boots to the UI, with working ADB and basic input/display |
| P4 Hardware completion | Functional matrix, fixes, and regression records | Every function has a PASS, FAIL, or NOT TESTED status |
| P5 Reproduction and CI | Locked manifest, build environment, scripts, CI | A new build directory can produce artifacts from the locked inputs |
| P6 Upgrade evaluation | Compatibility gaps and experimental results for the next version | Select a target from source evidence and hardware tests without promising complete hardware support |

Initially prioritize whether LineageOS 16.0 / Android 9 is suitable for the existing stock blobs. It is a candidate, not a mandatory baseline. If a complete and credible device tree for another version is easier to verify, document the evidence and adjust the baseline. Do not require a mechanical upgrade through every Android major version; an adjacent device's mature baseline or a cross-version migration is acceptable, but every step must preserve a usable baseline.

The first working ADB connection proves only that the boot chain has progressed. It does not mean the project is complete or that cameras, communications, or stability are solved.

## 3. Mandatory first-round work

1. Inspect the host OS, CPU, RAM, free disk space, filesystem, network, Git, Repo, Python, Java, build tools, and ADB. Do not print account tokens, keys, or complete sensitive environment variables.
2. Check whether the current directory is already a Git project. If not, establish the project in the directory specified by the user. Do not create a remote repository or push without being asked.
3. Inventory user-provided firmware, source, historical ROMs, and test devices. If multiple ADB devices are present, determine the intended serial and explicitly use `adb -s SERIAL` in every command.
4. Investigate the candidate sources below and record actual accessibility, branches, full commit SHAs, licenses, and device suitability.
5. Write `docs/inventory.md` and `docs/baseline-decision.md`, identifying the minimum viable path and the inputs that are genuinely missing.
6. Begin all executable work. If firmware is missing, finish source research and host checks first; if the device is missing, build first. Never record “untested” as “passed.”

Candidate sources are leads only and must be revalidated:

- https://github.com/vm03/android_device_samsung_c5lte
- https://github.com/vm03/android_kernel_samsung_c5lte
- https://github.com/vm03/proprietary_vendor_samsung_c5lte
- https://github.com/LineageOS/android
- https://opensource.samsung.com/
- https://4pda.to/forum/index.php?showtopic=743761

Inspect every remote branch, tag, commit history, and public fork of the device repositories; do not conclude that a device tree is absent after looking only at the default branch README. Historical ROMs may provide clues about boot, init, configuration, and blobs, but cannot replace reproducible evidence from corresponding source.

When referencing Qualcomm and Samsung devices from the same BSP generation, compare architecture, kernel ABI, graphics stack, audio, RIL, and partition layout. Similar SoC names do not imply that drivers or HALs are interchangeable. MSM8952, arm64, kernel 3.x, stock Android 8, and the existence of a flashable recovery were mentioned in earlier discussion but must all be independently confirmed.

## 4. Project organization

Use an orchestration repository plus separate source repositories. Keep the full Android worktree outside this project so hundreds of gigabytes of checkout data cannot be committed accidentally.

```text
 galaxy-c5-forge/
   README.md
   AGENTS.md
   manifests/                 # Development and locked manifests
   device/samsung/<confirmed-codename>/
   vendor-config/             # Blob inventories, extraction rules, hashes, and generation rules
   patches/                   # Organized by upstream project and baseline commit
   scripts/
   containers/
   ci/
   docs/
     inventory.md
     hardware.md
     baseline-decision.md
     build.md
     recovery-and-flashing.md
     test-matrix.md
     status.md
   .github/workflows/
```

If the kernel uses a separate fork, reference its fixed commit from the manifest; do not duplicate a huge source tree or use an inaccessible local path. A small device tree may live in the orchestration repository and be mapped into the Android worktree by the setup script. If several Git projects are maintained in parallel, identify the single authoritative source for each path in the README.

Ignore `.repo/`, `out/`, ccache, downloaded firmware, extracted blobs, logs, private keys, device backups, and user data. Store logs and artifacts in a separate artifact directory. Keep only concise, sanitized error records and analysis conclusions in Git.

## 5. Firmware and hardware reconstruction

Preserve a read-only copy of original firmware and record its model, region, version, source, acquisition date, and SHA-256. Record parser-tool versions as well. Never infer partition structure from filenames.

Inspect Samsung packaging and compression formats before choosing tar, LZ4, sparse-image, ext4, or other parsing tools. Prefer userspace extraction tools that do not execute firmware contents; do not directly mount an untrusted image read-write.

### boot / recovery

Extract and record the boot-header version, page size, kernel/ramdisk, DTB location and packaging, cmdline, field offsets or addresses, compression format, signature/trailer, and image size. Use only unpack/repack tools that match the actual image format.

Analyze init, fstab, ueventd, USB/ADB, encryption, SELinux, mount order, and Samsung-specific services in the ramdisk. An old device may lack a separate vendor partition, dtbo, A/B slots, or fastboot. Confirm each item and do not impose a modern-device template.

### Partitions and physical-device information

Collect available `getprop` output, by-name links, block-device sizes, mounts, kernel version, and configuration read-only. If PIT can be acquired safely and read-only, preserve and cross-check it. Compare firmware, fstab, and live-device data to confirm the real boot/recovery/system capacities.

Record evidence source paths and tool output. Never commit serial numbers, IMEI values, phone numbers, wireless addresses, or account data to a public repository.

### system / vendor and blobs

Confirm actual 32-bit and 64-bit ABIs and library directories. Scan HALs, services, firmware, configuration, SELinux, permission XML, and VINTF data when present. Use tools such as `readelf` to inspect ELF type, `DT_NEEDED`, and undefined symbols before generating a blob inventory and dependency closure.

For every blob, record the stock path, target path, hash, architecture, purpose, dependencies, and every modification. A patch must verify the original hash and record the modified hash. Do not implement “ignore it when missing” behavior or automatically substitute a library from another model.

By default, a public project should contain only extraction scripts, manifests, and the required patch descriptions. Whether a particular binary may be redistributed depends on its license and authorization. Firmware, blobs, modem files, and ROMs are not equivalent sources; do not mix unverified versions.

## 6. Kernel and minimal recovery

First reproduce the kernel corresponding to the firmware: inspect the Makefile version, Samsung build scripts, defconfig, DTS, toolchain, and compiler options. Determine whether the output appends a DTB or requires special packaging. Source may be incomplete or may not match production firmware; record any difference as a blocker.

For an old kernel, first establish a baseline with its matching toolchain rather than broadly rewriting the source because it fails under the newest Clang. A stock prebuilt kernel may be used as a staged diagnostic tool if its origin is explicit, but it cannot count as final kernel-source reproduction.

Build BoardConfig, product definitions, recovery fstab, init, and build targets from evidence; confirm product registration and the actual lunch name. Replace `<confirmed-codename>` through investigation instead of executing a guessed `lunch lineage_c5ltechn-userdebug` command.

Build the recovery target actually supported by the selected branch first. Check image size, header, DTB, fstab, and USB configuration, and generate a SHA-256. After a successful build, prepare flashing and recovery instructions before asking for authorization to operate on the physical device.

P2 validation requires a successful boot, explicit display/key/touch status, working ADB, correct partition discovery, and a clear reboot or stock-return path. Do not decrypt or erase userdata by default merely to validate recovery.

## 7. System bring-up and troubleshooting

Proceed in dependency order: kernel → init/mounts → ADB → linker/services → SurfaceFlinger/display → input → audio/network → cameras/communications/fingerprint → power and stability.

For every iteration, preserve the build SHA, image hash, test-device firmware version, reproduction steps, logs, and conclusion. When the device is unresponsive, collect recovery logs, pstore, or last_kmsg when available; these interfaces may not exist and kernel logs cannot be assumed obtainable.

- Linker errors: compare symbol definitions, calling conventions, and object layouts in the old and new sources. A shim must match the ABI and semantics. An empty implementation is acceptable only for a clearly isolated diagnostic experiment and does not count as a functional fix.
- HAL: inspect service registration, init, manifest, permissions, namespaces, dependencies, and kernel interfaces. Do not invent a manifest merely because a service cannot be found.
- SELinux: derive the smallest rules from actual AVCs and the call path. Do not paste bulk `audit2allow` output. Permissive mode is allowed only for isolated development diagnostics; a release must run enforcing and pass the real access test.
- Graphics/camera: inspect gralloc buffers, fences, ION/dma-buf, HAL interfaces, and kernel compatibility. Resolving symbols alone does not prove compatible data layouts.
- RIL/IMS: distinguish SIM detection, data, SMS, ordinary voice, VoLTE, and dual-SIM behavior. Working LTE data does not prove voice calling; results depend on carrier networks and IMS support, and 2G/3G fallback must not be promised.
- Kernel upgrades: list required userspace features and current gaps before selecting backports or a newer platform baseline. Estimate driver and DTS migration separately; do not replace the entire kernel merely because its version number is old.

## 8. Functional and regression matrix

For every item, record PASS / FAIL / NOT TESTED, artifact version, test steps, result, and log location. Cover at least:

- Cold boot, reboot, power-off, charging mode, recovery, and stock restoration.
- Display, brightness, touch, hardware keys, rotation, USB, and ADB.
- Wi-Fi, Bluetooth pairing and audio, GPS, sensors, and NFC after confirming the hardware.
- Speaker, earpiece/headset, microphone, recording, and bidirectional call audio.
- Front/rear cameras, still capture, recording, playback, and hardware codecs.
- Dual SIM, data, SMS send/receive, voice, and VoLTE as separate tests.
- Fingerprint enrollment/unlock, suspend/resume, deep sleep, temperature, and standby drain.
- Encryption, lock screen, SELinux enforcing, update installation, and data preservation.

Never call a real emergency number for validation. Release notes must explicitly identify untested functions and known failures. Do not infer an old kernel's patch level solely from the Android security-patch string.

## 9. Reproducible-build requirements

Describe two separate goals: A. locked inputs can rebuild the software and reproduce its functionality on hardware; B. two builds produce byte-identical artifacts. Complete A first, and claim B only after an actual comparison passes.

Pin every Repo project to a full commit SHA, export a locked manifest, and pin device source, kernel, patches, and blob inputs. Pin the build-container image digest, toolchain, and dependencies. Select the JDK, Python, and host environment for an old Android branch from that branch's source requirements; do not assume Ubuntu 24.04 or Python 3 works for every old branch.

Record the build target, command, environment, tool versions, artifact SHA-256, source lock, and logs. Containerization does not automatically eliminate timestamps, paths, signing differences, or randomness.

Suggested script interfaces, which are implementation targets rather than existing commands:

```bash
./scripts/doctor.sh
./scripts/setup.sh --workdir /absolute/android-work --manifest manifests/locked.xml
./scripts/extract-stock.sh --firmware /absolute/stock-package --output /absolute/vendor-output
./scripts/apply-patches.sh --workdir /absolute/android-work
./scripts/build.sh --workdir /absolute/android-work --target recovery
./scripts/build.sh --workdir /absolute/android-work --target rom
./scripts/collect-logs.sh --serial DEVICE_SERIAL --output /absolute/test-logs
```

Scripts must validate input hashes, paths, available space, and targets and return nonzero on failure. Repeated setup/extraction must produce the same result. Patches must validate their baseline and recognize that they are already applied; conflicts must not be skipped silently. Never automate `git reset --hard`, `git clean -xfd`, or recursive deletion of a user directory.

For byte-level reproduction, build in two independent clean output directories, normalize every build-supported date, build number, username, and path, then compare SHA-256 values. If they differ, unpack and compare the artifacts. Keep signing private keys only in secret storage and never commit them. Record officially signed and test-signed artifacts separately.

## 10. CI design

Measure actual project duration before setting budgets; estimates such as 10–40 or 30–90 minutes from earlier discussion are guesses, not acceptance promises. Time Repo synchronization, blob extraction, kernel build, recovery build, and ROM packaging separately, distinguishing cold builds, warm-cache rebuilds, and incremental builds.

| Workflow | Trigger | Contents |
| --- | --- | --- |
| check | PR / commit | Script syntax, manifest/hash format, product configuration, patch baseline checks |
| recovery | Manual or trusted branch | Real recovery build and image checks for the selected branch |
| rom | Manual / release | Complete locked-source build, artifacts, and provenance |
| clean-rebuild | Scheduled or manual | Independent clean output build and reproducibility validation |
| hardware | Explicitly authorized trusted artifact | Validation and log collection on the designated test device |

Ordinary hosted runners are suitable for lightweight checks. Before using one for a complete Android build, verify its current disk, memory, and time limits. A self-hosted runner may initially be planned around 16–32 CPU threads, 64 GiB RAM, and 1 TiB NVMe, then adjusted from branch-specific measurements; these are not guaranteed minimum requirements.

Confirm ccache support for the selected branch rather than assuming that setting `USE_CCACHE` makes it work. Preserving a cache and reusing `out` are different mechanisms. A formal clean validation must not depend on an old output tree. Lock persistent workspaces to prevent concurrent-build contamination and ensure inputs return to their locked revisions.

Do not run an untrusted fork PR directly on a self-hosted runner that has devices, signing keys, or long-lived credentials. Isolate lightweight PR checks and restrict complete builds to trusted revisions. Use minimum GitHub Actions permissions, pin third-party actions to commits, and separate signing from device runners.

Hardware workflows must disable writes by default. Even when enabled, require serial/model checks, image hash and size checks, timeouts, a recovery path, and an explicit partition allowlist. Never include bootloader, modem, EFS, PIT/repartition in routine automation. Preserve logs on failure and do not loop through blind flashing attempts.

## 11. Deliverables before flashing

Produce a concrete review bundle: test-device model/serial, current firmware/bootloader, OEM/FRP state, original-backup status, artifact name/hash/size, target partitions, exact commands or manual flow, boot-failure recovery path, and whether user data is affected.

Determine Samsung flashing methods from actual device capability. Do not assume fastboot exists or that Heimdall supports every operation on this device. If the Odin GUI is required, provide reviewable instructions and wait for the user to perform or authorize control of the operation.

Store and protect backups of EFS and other sensitive partitions separately; do not upload them to public CI and never restore another device's backup to this device. Without explicit authorization, do not modify the bootloader or modem, repartition, or disable safety checks to hide a format error.

## 12. Final delivery standard

The repository must contain a clear model and support scope, locked source manifest, usable kernel source, blob extraction rules and hashes, executable scripts, build environment, installation and recovery instructions, CI, test matrix, known issues, and license information.

Complete at least one build from a fresh Android worktree and validate the corresponding milestone on the designated device. Every release bundle must include complete provenance, SHA-256 values, and a log index. Without hardware testing, the project can be delivered only as “built, not verified.”

Modern Android is a later target. First investigate its current build, kernel, and HAL requirements, then provide a gap analysis and experimental route. Never claim Android 15/16 support merely because it compiles, and do not promise that every proprietary hardware interface can be repaired.

## 13. Ready-to-paste local Codex kickoff task

> Read `galaxy-c5-forge-codex-plan.md` in this directory and use it as the execution constraints for Galaxy C5 Forge. Complete P0 first and continue all work that does not depend on missing inputs. Inspect the host and current repository, investigate the real source, firmware, and device information for the Samsung Galaxy C5 SM-C5000, validate every branch and history of the candidate repositories, and select the smallest recovery baseline. Produce `docs/inventory.md`, `docs/baseline-decision.md`, and `docs/status.md`, and implement executable project scripts. My first hardware target is a booting recovery with ADB; the final target is a reproducible unofficial LineageOS project with CI. Do not merely write a plan, and do not fabricate builds or hardware-test results. Flashing, erasure, and device-data modification are not yet authorized; when the device or firmware is missing, continue software-side work and end with the minimum required inputs. Leave an accurate status and next executable action after every session.
