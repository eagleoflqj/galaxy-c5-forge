#!/usr/bin/env python3
"""Inventory measured C5 GCC supplies and legacy SMMU graph, not driver compatibility."""
import argparse
import importlib.util
import json
import re
import struct
from pathlib import Path

parser_path = Path(__file__).with_name("kernel-migration-audit.py")
spec = importlib.util.spec_from_file_location("kernel_migration_audit", parser_path)
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)

STRINGS = {"regulator-name", "qcom,resource-name", "reg-names", "clock-names",
           "rpm-channel-name", "qcom,rpm-channel-name"}
CELLS = {"reg", "interrupts", "clocks", "qcom,cb-base-offset", "qcom,iommu-secure-id",
         "qcom,iommu-ctx-sids", "qcom,iommu-sid-mask", "qcom,virtual-addr-pool",
         "qcom,resource-id", "qcom,regulator-type", "qcom,set", "rpm-channel-type",
         "qcom,rpm-channel-type", "regulator-min-microvolt", "regulator-max-microvolt"}
FLAGS = {"qcom,use-voltage-level", "qcom,use-voltage-floor-level", "qcom,secure-context",
         "qcom,secure-domain", "qcom,report-iommu-faults", "qcom,always-send-voltage"}


def cells(value):
    if len(value) % 4:
        raise ValueError("not a whole number of FDT cells")
    return list(struct.unpack(f">{len(value) // 4}I", value))


def strings(value):
    if not value or not value.endswith(b"\0"):
        raise ValueError("unterminated FDT string property")
    return value[:-1].decode("ascii").split("\0")


def describe(node):
    result = {key: node[key] for key in ("path", "compatible", "status", "enabled_with_ancestors")}
    result["properties"] = {}
    for name, value in sorted(node["properties"].items()):
        if name in STRINGS:
            result["properties"][name] = strings(value)
        elif name in CELLS:
            result["properties"][name] = cells(value)
        elif name in FLAGS:
            if value:
                raise ValueError(f"nonempty boolean property: {node['path']}:{name}")
            result["properties"][name] = True
    return result


def inventory(nodes):
    by_path = {node["path"]: node for node in nodes}
    if len(by_path) != len(nodes):
        raise ValueError("duplicate DT node path")
    handles = {}
    for node in nodes:
        aliases = [cells(node["properties"][name]) for name in ("phandle", "linux,phandle")
                   if name in node["properties"]]
        if not aliases:
            continue
        if any(len(value) != 1 or not 0 < value[0] < 0xffffffff for value in aliases):
            raise ValueError("invalid phandle")
        if any(value != aliases[0] for value in aliases):
            raise ValueError("disagreeing phandle aliases")
        handle = aliases[0][0]
        if handle in handles:
            raise ValueError("duplicate phandle")
        handles[handle] = node

    def resolve(value):
        if value not in handles:
            raise ValueError(f"unresolved phandle: {value:#x}")
        return handles[value]

    gcc = by_path["/soc/qcom,gcc@1800000"]
    supplies = []
    for name, value in sorted(gcc["properties"].items()):
        if not name.endswith("-supply"):
            continue
        refs = cells(value)
        if len(refs) != 1:
            raise ValueError("supply must contain exactly one phandle")
        provider = resolve(refs[0])
        parent = by_path[provider["path"].rsplit("/", 1)[0] or "/"]
        supplies.append({"property": name, "phandle": refs[0], "provider": describe(provider),
                         "resource_parent": describe(parent)})
    controllers, contexts, domains = [], [], []
    for node in nodes:
        if "qcom,msm-smmu-v2" in node["compatible"]:
            controllers.append(describe(node))
        if "qcom,msm-smmu-v2-ctx" in node["compatible"]:
            entry = describe(node)
            parent = by_path[node["path"].rsplit("/", 1)[0]]
            if "qcom,msm-smmu-v2" not in parent["compatible"]:
                raise ValueError("legacy context without legacy controller")
            entry["controller"] = parent["path"]
            sids = cells(node["properties"]["qcom,iommu-ctx-sids"])
            masks = cells(node["properties"]["qcom,iommu-sid-mask"]) if "qcom,iommu-sid-mask" in node["properties"] else None
            if not sids or (masks is not None and len(masks) != len(sids)):
                raise ValueError("invalid legacy SID/mask array lengths")
            # Preserve absent masks as null. Interpretation belongs to driver review.
            entry["sid_count"] = len(sids)
            entry["explicit_sid_masks"] = masks
            contexts.append(entry)
        if "qcom,iommu-contexts" in node["properties"]:
            entry = describe(node)
            entry["context_paths"] = []
            for value in cells(node["properties"]["qcom,iommu-contexts"]):
                target = resolve(value)
                if "qcom,msm-smmu-v2-ctx" not in target["compatible"]:
                    raise ValueError("domain phandle is not a legacy context")
                entry["context_paths"].append(target["path"])
            domains.append(entry)
    return {"scope": "MEASURED_BINDING_INVENTORY_NOT_COMPATIBILITY_OR_HARDWARE_PASS",
            "gcc": describe(gcc), "gcc_supplies": supplies,
            "rpm_transport": describe(by_path["/soc/qcom,rpm-smd"]),
            "smmu_controllers": controllers, "smmu_contexts": contexts, "legacy_domains": domains,
            "counts": {"gcc_supplies": len(supplies), "controllers": len(controllers),
                       "contexts": len(contexts), "domains": len(domains),
                       "secure_contexts": sum("qcom,secure-context" in entry["properties"] for entry in contexts),
                       "contexts_with_explicit_masks": sum(entry["explicit_sid_masks"] is not None for entry in contexts)},
            "open": ["RPM transport/channel and actual regulator acknowledgements",
                     "Complete supply graph, SPMI/CPR/SPM/memory-acceleration integration",
                     "Legacy context banks/IRQs, SID masks, domains and consumer API migration",
                     "Secure monitor/VMID ABI and live DMA fault/isolation tests"]}


def match_table(path, name):
    text = path.read_text()
    block = re.search(r"static\s+(?:const\s+)?struct of_device_id\s+" + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not block:
        raise ValueError(f"missing match table: {path}:{name}")
    return {"path": str(path), "sha256": parser.sha256(path), "table": name,
            "compatible": re.findall(r'\.compatible\s*=\s*"([^"\n]+)"', block[1])}


def main():
    args_parser = argparse.ArgumentParser(description=__doc__)
    args_parser.add_argument("dtb", type=Path)
    args_parser.add_argument("candidate", type=Path)
    args = args_parser.parse_args()
    report = inventory(parser.dt_nodes(args.dtb.read_bytes(), include_properties=True))
    report["inputs"] = {"dtb_path": str(args.dtb), "dtb_sha256": parser.sha256(args.dtb),
                        "auditor_sha256": parser.sha256(Path(__file__)), "parser_sha256": parser.sha256(parser_path)}
    report["candidate_match_tables"] = [
        match_table(args.candidate / "drivers/iommu/arm-smmu.c", "arm_smmu_of_match"),
        match_table(args.candidate / "drivers/regulator/rpm-smd-regulator.c", "rpm_vreg_match_table_device"),
        match_table(args.candidate / "drivers/regulator/rpm-smd-regulator.c", "rpm_vreg_match_table_resource")]
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
