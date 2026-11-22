#!/usr/bin/env python3
"""Read-only, pinned C5 early-memory/PSTORE review; never a hardware boot gate."""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path

spec = importlib.util.spec_from_file_location("board", Path(__file__).with_name("kernel-c5-board-dt.py"))
board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board)

DTB_SHA = "396f104f2e9ad30ed83cca198cdec4eaec8def965c2712d1a2abf6a3f0c45ad8"
NATIVE_PINS = {
    "drivers/of/fdt.c": "03f837edee179b503f74599ddd8b4a1d5bfc362d22f9045a4d01e5f49d9e86b6",
    "drivers/of/platform.c": "b97f98e203ebe990fb0beb4b2f7742912cb1b7ef34c30fe3edb73ca2267831c4",
    "drivers/base/dma-contiguous.c": "eea91ec58ef81d10396de6c65fc291465e0b812870c49e12a96cebe2cbea39d7",
    "fs/pstore/ram.c": "34e895eccc3b98909ba79e04425d6aff4419faf597770ed2f505b77722370cc6",
}
LEGACY_CMA_SHA = "0aa08b648f4c9b0c465bf541a613c79b563a96f1394a0890f25eefb2370670be"
REQUIRED_CONFIG = ("CGROUP_CPUACCT", "PSTORE", "PSTORE_RAM", "PSTORE_CONSOLE", "PSTORE_PMSG")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def single(props, key):
    raw = props.get(key)
    require(raw is not None and len(raw) == 4, f"Expected one cell: {key}")
    return struct.unpack(">I", raw)[0]


def reg_ranges(props, address_cells, size_cells):
    require(address_cells in (1, 2) and size_cells in (1, 2), "Unsupported memory cell width")
    raw = props.get("reg")
    width = 4 * (address_cells + size_cells)
    require(raw is not None and len(raw) > 0 and len(raw) % width == 0, "Malformed memory reg")
    ranges = []
    for offset in range(0, len(raw), width):
        split = offset + 4 * address_cells
        base = int.from_bytes(raw[offset:split], "big")
        size = int.from_bytes(raw[split:offset + width], "big")
        require(size > 0 and base + size <= 1 << 64, "Empty or overflowing memory range")
        ranges.append({"base": base, "size": size, "end_exclusive": base + size})
    return ranges


def reserved_root_issues(nodes):
    root = nodes["/"]
    reserved = nodes.get("/reserved-memory")
    if reserved is None:
        return ["Missing /reserved-memory"]
    issues = []
    for name in ("#address-cells", "#size-cells"):
        expected = single(root, name)
        if reserved.get(name) != struct.pack(">I", expected):
            issues.append(f"/reserved-memory {name} missing or differs from root")
    # Native check requires presence; this audit additionally refuses translation.
    if reserved.get("ranges") != b"":
        issues.append("/reserved-memory requires an explicit empty ranges property")
    return issues


def enabled(props):
    return props.get("status", b"okay\0") in (b"ok\0", b"okay\0")


def covered(base, end, ranges):
    """Coverage by a union, not merely an overlap or a single containing tuple."""
    cursor = base
    for left, right in sorted(ranges):
        if left > cursor:
            break
        if right > cursor:
            cursor = right
        if cursor >= end:
            return True
    return False


def audit(nodes, config_text, reserve_map):
    root_issues = reserved_root_issues(nodes)
    ac = single(nodes["/"], "#address-cells")
    sc = single(nodes["/"], "#size-cells")
    # A structural model of the pinned native scan, not execution of memblock.
    native_ranges = []
    if not root_issues:
        for path, props in nodes.items():
            if path.startswith("/reserved-memory/") and path.count("/") == 2 and enabled(props) and "reg" in props:
                for area in reg_ranges(props, ac, sc):
                    native_ranges.append({"path": path, "no_map": "no-map" in props, **area})
    header_ranges = [(base, base + size) for base, size in reserve_map]
    require(all(size > 0 and base + size <= 1 << 64 for base, size in reserve_map), "Invalid reserve-map extent")
    modeled_coverage = header_ranges + [(area["base"], area["end_exclusive"]) for area in native_ranges]
    legacy, fixed_missing, dynamic_missing, removal_missing, overlaps = [], [], [], [], []
    removed_coverage = [(area["base"], area["end_exclusive"]) for area in native_ranges if area["no_map"]]
    for path, props in nodes.items():
        if "linux,reserve-contiguous-region" not in props or path.count("/") != 2 or not enabled(props):
            continue
        phandle = board.handle(props) if "phandle" in props or "linux,phandle" in props else None
        consumers = []
        if phandle is not None:
            for consumer_path, consumer_props in nodes.items():
                for name in ("linux,contiguous-region", "memory-region"):
                    if name in consumer_props and phandle in board.words(consumer_props[name]):
                        consumers.append({"path": consumer_path, "property": name})
        for area in reg_ranges(props, ac, sc):
            dynamic = area["base"] == 0
            entry = {"path": path, **area, "legacy_remove_completely": "linux,remove-completely" in props,
                     "legacy_reserved_not_reusable": "linux,reserve-region" in props,
                     "legacy_dynamic_base": dynamic,
                     "legacy_memory_limit_cells": board.words(props.get("linux,memory-limit", b"")),
                     "phandle": phandle, "consumer_edges": consumers,
                     "covered_by_native_static_model": not dynamic and covered(area["base"], area["end_exclusive"], modeled_coverage)}
            legacy.append(entry)
            if dynamic:
                dynamic_missing.append(path)
            elif not entry["covered_by_native_static_model"]:
                fixed_missing.append(path)
            if not dynamic and entry["legacy_remove_completely"] and not covered(area["base"], area["end_exclusive"], removed_coverage):
                removal_missing.append(path)
    for index, left in enumerate(legacy):
        for right in legacy[index + 1:]:
            if left["legacy_dynamic_base"] or right["legacy_dynamic_base"]:
                continue
            if max(left["base"], right["base"]) < min(left["end_exclusive"], right["end_exclusive"]):
                overlaps.append([left["path"], right["path"]])
    # of_find_compatible_node starts AFTER the supplied node, not within it.
    paths = list(nodes)
    start = paths.index("/reserved-memory") if "/reserved-memory" in nodes else len(paths)
    discovered = next((path for path in paths[start + 1:] if "ramoops" in board.string_values(nodes[path].get("compatible"))), None)
    ramoops = [path for path, props in nodes.items() if "ramoops" in board.string_values(props.get("compatible"))]
    missing_config = [name for name in REQUIRED_CONFIG if f"CONFIG_{name}=y" not in config_text.splitlines()]
    blockers = list(root_issues)
    if fixed_missing:
        blockers.append("Legacy fixed carveouts lack native structural reservation coverage")
    if dynamic_missing:
        blockers.append("Legacy zero-base CMA allocations need explicit native allocation/consumer review; zero is not a physical address")
    if removal_missing:
        blockers.append("Legacy remove-completely carveouts lack native no-map structural coverage")
    if overlaps:
        blockers.append("Overlapping legacy carveouts require explicit ownership/alias review")
    if ramoops != [discovered]:
        blockers.append("Native platform discovery does not select the single legacy ramoops node")
    if missing_config:
        blockers.append("Missing recovery mount/logging configuration: " + ", ".join(missing_config))
    return {
        "status": "HOLD_NOT_READY_FOR_DEVICE_TEST" if blockers else "STRUCTURAL_REVIEW_ONLY_NOT_BOOT_ACCEPTANCE",
        "meaning": "PINNED_SOURCE_DT_CONFIG_MODEL_NOT_EXECUTED_KERNEL_OR_BOOTLOADER_MEMORY_MAP",
        "hardware": "NOT TESTED", "blockers": blockers,
        "reserved_root_issues": root_issues, "header_reserve_map": reserve_map,
        "native_structurally_scannable_ranges": native_ranges,
        "legacy_regions": legacy, "fixed_regions_missing_native_coverage": fixed_missing,
        "dynamic_regions_pending_native_allocation_review": dynamic_missing,
        "fixed_remove_regions_missing_native_no_map": removal_missing,
        "overlapping_legacy_regions": overlaps,
        "ramoops_nodes": ramoops, "native_platform_search_result": discovered,
        "missing_config": missing_config,
        "not_proven": ["Bootloader final RAM/DT fixups", "Actual early memblock reserve/remove success",
                       "CMA allocation/consumer mapping and secure ownership", "Persistent RAM contents and crash readback",
                       "Native no-map/cache attributes", "Any hardware safety or acceptance"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--native-source", type=Path, required=True)
    parser.add_argument("--legacy-source", type=Path, required=True)
    args = parser.parse_args()
    try:
        require(all(path.is_absolute() for path in vars(args).values()), "Absolute input paths required")
        require(sha(args.dtb) == DTB_SHA, "Unexpected reviewed full-board DTB")
        inputs = {str(args.dtb): sha(args.dtb), str(args.config): sha(args.config), str(Path(__file__).resolve()): sha(Path(__file__).resolve())}
        for name, digest in NATIVE_PINS.items():
            path = args.native_source / name
            require(sha(path) == digest, f"Unexpected native source contract: {name}")
            inputs[str(path)] = digest
        legacy = args.legacy_source / "drivers/base/dma-contiguous.c"
        require(sha(legacy) == LEGACY_CMA_SHA, "Unexpected legacy CMA contract")
        inputs[str(legacy)] = LEGACY_CMA_SHA
        result = audit(board.tree(args.dtb), args.config.read_text(), board.layout(args.dtb)["memory_reservations"])
        result["inputs_sha256"] = inputs
        result["inputs_sha256"][str(Path(board.__file__).resolve())] = sha(Path(board.__file__).resolve())
        result["inputs_sha256"][str(Path(board.migration.__file__).resolve())] = sha(Path(board.migration.__file__).resolve())
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1 if result["blockers"] else 0
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(2, f"ERROR: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
