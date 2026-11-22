#!/usr/bin/env python3
"""Inventory source-path/config gaps; absence is not proof of unsupported hardware."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def symbols(root):
    result = set()
    for path in root.rglob("*Kconfig*"):
        if path.is_file() and not path.is_symlink():
            result.update(re.findall(r"^\s*(?:menuconfig|config)\s+(\w+)", path.read_text(errors="replace"), re.M))
    return result


def compiled_sources(build):
    result = set()
    for path in build.rglob(".*.cmd"):
        for value in re.findall(r"^source_\S+\s*:=\s*(\S+)", path.read_text(errors="replace"), re.M):
            if value.startswith("/kernel/"):
                result.add(value[len("/kernel/"):])
    return sorted(result)


def dt_nodes(blob, include_properties=False):
    """Read a v17 FDT; optionally retain all nodes and their raw properties."""
    if len(blob) < 40:
        raise ValueError("truncated FDT header")
    magic, total, off_struct, off_strings, _, version, _, _, strings_size, struct_size = struct.unpack_from(
        ">10I", blob)
    if magic != 0xd00dfeed or version != 17 or total > len(blob):
        raise ValueError("unexpected FDT header")
    if off_struct + struct_size > total or off_strings + strings_size > total:
        raise ValueError("FDT block out of bounds")
    data = blob[off_struct:off_struct + struct_size]
    strings = blob[off_strings:off_strings + strings_size]
    nodes, stack, offset = [], [], 0
    while offset + 4 <= len(data):
        token, = struct.unpack_from(">I", data, offset)
        offset += 4
        if token == 1:
            end = data.index(0, offset)
            name = data[offset:end].decode("ascii")
            parent = stack[-1]["path"] if stack else ""
            node = {"path": parent.rstrip("/") + "/" + name, "compatible": [], "status": "okay"}
            if include_properties:
                node["properties"] = {}
            nodes.append(node)
            stack.append(node)
            offset = (end + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unmatched FDT end node")
            stack.pop()
        elif token == 3:
            if not stack or offset + 8 > len(data):
                raise ValueError("invalid FDT property")
            length, nameoff = struct.unpack_from(">II", data, offset)
            offset += 8
            if nameoff >= len(strings) or offset + length > len(data):
                raise ValueError("FDT property out of bounds")
            name = strings[nameoff:strings.index(0, nameoff)].decode("ascii")
            value = data[offset:offset + length]
            if include_properties:
                if name in stack[-1]["properties"]:
                    raise ValueError("duplicate FDT property")
                stack[-1]["properties"][name] = value
            if name == "compatible":
                stack[-1][name] = value.rstrip(b"\0").decode("ascii").split("\0")
            elif name == "status":
                stack[-1][name] = value.rstrip(b"\0").decode("ascii")
            offset = (offset + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            if stack:
                raise ValueError("unterminated FDT node")
            by_path = {node["path"]: node for node in nodes}
            for node in nodes:
                ancestors = ["/"] + [node["path"].rsplit("/", 1)[0] or "/"]
                parent = ancestors[-1]
                while parent != "/":
                    parent = parent.rsplit("/", 1)[0] or "/"
                    ancestors.append(parent)
                node["enabled_with_ancestors"] = all(
                    by_path[path]["status"] in ("okay", "ok") for path in set(ancestors + [node["path"]]))
            return nodes if include_properties else [node for node in nodes if node["compatible"]]
        else:
            raise ValueError(f"unknown FDT token {token}")
    raise ValueError("missing FDT end token")


def compatible_literals(root, requested):
    """Literal C-string occurrences, not proof of a registered or enabled driver."""
    result = {value: [] for value in sorted(requested)}
    for path in root.rglob("*.c"):
        if path.is_symlink():
            continue
        content = path.read_text(errors="replace")
        for value in set(re.findall(r'"([^"\n\\]*)"', content)) & requested:
            result[value].append(str(path.relative_to(root)))
    return {value: sorted(paths) for value, paths in result.items()}


def decode_probe(directory, tree, arch):
    binary = directory / f"{tree}-{arch}-probe.bin"
    symbol_file = directory / f"{tree}-{arch}-symbols.txt"
    data = binary.read_bytes()
    values, offsets = {}, set()
    pattern = r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+OBJECT\s+\S+\s+\S+\s+\d+\s+(probe_\w+)\s*$"
    for address, size, name in re.findall(pattern, symbol_file.read_text(), re.M):
        offset = int(address, 16)
        if size != "8" or offset % 8 or offset + 8 > len(data) or offset in offsets or name in values:
            raise ValueError("invalid probe symbol layout")
        offsets.add(offset)
        values[name] = struct.unpack_from("<Q", data, offset)[0]
    if not values or offsets != set(range(0, len(data), 8)):
        raise ValueError("incomplete probe symbol coverage")
    if values["probe_pointer_size"] != {"arm": 4, "arm64": 8}[arch]:
        raise ValueError("unexpected probe pointer size or endianness")
    return {"values": values, "binary_sha256": sha256(binary), "symbols_sha256": sha256(symbol_file)}


def compare_probes(directory):
    result = {}
    for arch in ("arm", "arm64"):
        pair = {name: decode_probe(directory, name, arch) for name in ("baseline", "reference")}
        before, after = (pair[name]["values"] for name in ("baseline", "reference"))
        if before.keys() != after.keys():
            raise ValueError("probe symbols differ between inputs")
        pair["differences"] = {key: {"baseline": value, "reference": after[key]}
                               for key, value in before.items() if value != after[key]}
        pair["meaning"] = "Selected header constants/layouts only; delta is not proof of runtime incompatibility."
        result[arch] = pair
    return result


def compare_config(baseline, candidate):
    values = dict(re.findall(r"^CONFIG_(\w+)=(.*)$", candidate.read_text(), re.M))
    enabled = set(re.findall(r"^CONFIG_(\w+)=[ym]$", baseline.read_text(), re.M))
    return {
        "candidate_config_sha256": sha256(candidate),
        "baseline_enabled_symbols_no_longer_enabled": sorted(key for key in enabled if values.get(key) not in ("y", "m")),
        "meaning": "Diagnostic olddefconfig result only; not a reviewed device configuration or build pass.",
    }


def inspect_tree(root, enabled, active):
    makefile = root / "Makefile"
    text = makefile.read_text()
    version = ".".join(re.search(r"^" + field + r"\s*=\s*(\d+)", text, re.M).group(1)
                       for field in ("VERSION", "PATCHLEVEL", "SUBLEVEL"))
    dts = sorted(str(path.relative_to(root)) for path in root.rglob("*8952*.dts*"))
    return {
        "version": version,
        "makefile_sha256": sha256(makefile),
        "enabled_baseline_symbols_without_same_named_declaration": sorted(enabled - symbols(root)),
        "compiled_baseline_sources_without_same_path": [path for path in active if not (root / path).is_file()],
        "msm8952_named_dts_inputs": dts,
        "c5lte_named_inputs": sorted(str(path.relative_to(root)) for path in root.rglob("*c5lte*")),
        "interface_paths": {path: (root / path).exists() for path in (
            "drivers/usb/gadget/function/f_fs.c", "drivers/usb/gadget/f_fs.c",
            "drivers/gpu/msm", "drivers/video/msm/mdss", "drivers/staging/android/ashmem.c",
            "drivers/staging/android/ion", "drivers/ion", "drivers/misc/qseecom.c",
            "drivers/soc/qcom/subsystem_restart.c", "drivers/platform/msm/ipa/ipa.c",
            "drivers/platform/msm/ipa/ipa_v2/ipa.c", "include/uapi/linux/msm_kgsl.h",
            "include/linux/msm_kgsl.h", "include/uapi/linux/qseecom.h")},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--abi-probe", type=Path, help="optional completed kernel-abi-probe.sh output")
    parser.add_argument("--candidate-config", type=Path, help="optional diagnostic olddefconfig output")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    config = args.baseline / "metadata/accepted.config"
    if sha256(config) != "183aa914ec1e08e0680bdabb58f810d2813ac44f8540225ec3337b9562a1b787":
        parser.error("unexpected baseline config")
    if args.output.exists() or args.output.is_symlink():
        parser.error("refusing to overwrite an existing report")
    enabled = set(re.findall(r"^CONFIG_(\w+)=[ym]$", config.read_text(), re.M))
    active = compiled_sources(args.baseline / "build")
    if not active:
        parser.error("no completed baseline build command records")
    result = {
        "meaning": "Textual source-path/Kconfig inventory only; no ABI, bootability or security pass is inferred.",
        "baseline_config_sha256": sha256(config),
        "baseline_enabled_boolean_or_module_symbols": len(enabled),
        "baseline_compiled_sources": active,
        "trees": {name: inspect_tree(root, enabled, active) for name, root in (
            ("baseline", args.baseline / "source"), ("upstream", args.upstream), ("qcom_reference", args.reference))},
    }
    dtb = args.baseline / "build/arch/arm64/boot/dts/msm8952-sec-c5lte-chn-hk-r09.dtb"
    nodes = dt_nodes(dtb.read_bytes())
    requested = {value for node in nodes for value in node["compatible"]}
    result["board"] = {
        "dtb_sha256": sha256(dtb), "nodes": nodes,
        "meaning": "Literal C-string hits only, including comments; review binding semantics, Kbuild selection and probes separately.",
        "compatible_literal_hits": {name: compatible_literals(root, requested) for name, root in (
            ("baseline", args.baseline / "source"), ("upstream", args.upstream), ("qcom_reference", args.reference))},
    }
    if args.abi_probe:
        result["selected_abi_probe"] = compare_probes(args.abi_probe)
    if args.candidate_config:
        result["diagnostic_config"] = compare_config(config, args.candidate_config)
    if result["trees"]["upstream"]["version"] != "4.9.337" or result["trees"]["qcom_reference"]["version"] != "4.9.337":
        parser.error("unexpected target/reference version")
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
    for name, tree in result["trees"].items():
        print(f"{name}: version={tree['version']} missing_same_named_symbols="
              f"{len(tree['enabled_baseline_symbols_without_same_named_declaration'])} "
              f"missing_same_path_sources={len(tree['compiled_baseline_sources_without_same_path'])}/{len(active)} "
              f"msm8952_dts_inputs={len(tree['msm8952_named_dts_inputs'])}")
    print(f"report_sha256={sha256(args.output)}")


if __name__ == "__main__":
    main()
