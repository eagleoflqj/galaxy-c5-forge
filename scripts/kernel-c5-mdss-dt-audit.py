#!/usr/bin/env python3
"""Validate source-defined C5 display IOVA pools and measured context metadata only."""
import argparse
import importlib.util
import json
import re
from pathlib import Path

provider_path = Path(__file__).with_name("kernel-c5-provider-audit.py")
spec = importlib.util.spec_from_file_location("mdss_provider", provider_path)
provider = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provider)
IDENTITY = "samsung,c5lte-mdss-domain-host-fixture"
BASELINE_SHA = "83e02661e33cc35a25c6fee66abb0e2f492e2dc854b79a396a8eb90817869713"
SOURCE_SHA = "e0c3f6281e61fbe63d4149a648744afd04df0d28f4e77149f0c077c532897fc2"
SIZES_SHA = "49708873dcde4ce1e6f5a8721dfd39f0ff65ab613a06b7d11a813db9a1e28fe5"
CONSUMER = "/soc/qcom,mdss_mdp@1a00000"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def text(props, name):
    values = provider.strings(props[name])
    require(len(values) == 1, f"Expected one string: {name}")
    return values[0]


def one(props, name):
    values = provider.cells(props[name])
    require(len(values) == 1, f"Expected one cell: {name}")
    return values[0]


def unique(source, pattern):
    matches = re.findall(pattern, source, re.M | re.S)
    require(len(matches) == 1, f"Missing/ambiguous source contract: {pattern}")
    return matches[0]


def source_contract(source, sizes):
    # No eval or source execution. Only the two pinned legacy expressions are accepted.
    constants = {name: int(unique(sizes, rf"^#define {name}\s+(0x[0-9a-fA-F]+)\s*$"), 16)
                 for name in ("SZ_128K", "SZ_1G")}
    array = unique(source, r"^struct mdss_iommu_map_type mdss_iommu_map\[MDSS_IOMMU_MAX_DOMAIN\] = \{(.*?)^\};")
    require(array.count(".npartitions = 1") == 2, "Unexpected legacy display partition count")
    require("layout.is_secure = (i == MDSS_IOMMU_DOMAIN_SECURE);" in source,
            "Missing legacy secure-domain policy")
    require("iomap->domain_idx = msm_register_domain(&layout);" in source,
            "Missing dynamic legacy domain registration")
    contracts = []
    for suffix in ("UNSECURE", "SECURE"):
        block = unique(array, rf"\[MDSS_IOMMU_DOMAIN_{suffix}\] = \{{(.*?)\n\t\}},")
        start = unique(block, r"\.start = (SZ_128K|SZ_1G),")
        size = unique(block, r"\.size = (SZ_1G - SZ_128K|SZ_1G),")
        contracts.append({"context_name": unique(block, r'\.ctx_name = "([^"\n]+)"'),
                          "client_name": unique(block, r'\.client_name = "([^"\n]+)"'),
                          "pool": [constants[start], constants["SZ_1G"] - constants["SZ_128K"]
                                   if size == "SZ_1G - SZ_128K" else constants["SZ_1G"]],
                          "secure_domain": suffix == "SECURE"})
    return contracts


def validate(baseline, fixture, contracts):
    old = {node["path"]: node for node in baseline}
    nodes = {node["path"]: node for node in fixture}
    require(len(old) == len(baseline) and len(nodes) == len(fixture), "Duplicate DT path")
    require(nodes["/"]["compatible"] == [IDENTITY], "Wrong MDSS host fixture identity")
    require(set(nodes) == {"/", "/providers", "/providers/apps", "/display",
                           "/display/mdp-unsecure", "/display/mdp-secure"}, "Unexpected fixture topology")
    handles = {}
    for node in fixture:
        require(node["enabled_with_ancestors"], "Disabled fixture node/ancestor")
        require(not node["compatible"] or node["path"] == "/", "Runtime compatible in host fixture")
        require(not any(name in node["properties"] for name in ("reg", "interrupts", "clocks", "clock-names")),
                "Runtime resources in evidence-only fixture")
        aliases = [one(node["properties"], name) for name in ("phandle", "linux,phandle") if name in node["properties"]]
        if aliases:
            require(all(value == aliases[0] for value in aliases) and 0 < aliases[0] < 0xffffffff and
                    aliases[0] not in handles, "Invalid/duplicate fixture phandle aliases")
            handles[aliases[0]] = node
    require(text(nodes["/display"]["properties"], "forge,baseline-consumer") == CONSUMER,
            "Changed baseline consumer identity")
    require(old[CONSUMER]["enabled_with_ancestors"] and "qcom,mdss_mdp" in old[CONSUMER]["compatible"],
            "Missing/disabled legacy MDSS consumer")
    pnode = nodes["/providers/apps"]["properties"]
    require(one(pnode, "#iommu-cells") == 2, "Expected SID/mask provider")
    aliases = [one(pnode, name) for name in ("phandle", "linux,phandle") if name in pnode]
    require(bool(aliases) and all(value == aliases[0] for value in aliases) and 0 < aliases[0] < 0xffffffff,
            "Invalid provider phandle aliases")
    handle = aliases[0]
    require(handles[handle] is nodes["/providers/apps"], "Wrong fixture provider")
    controller_path = text(pnode, "forge,baseline-controller")
    controller = old[controller_path]
    require(controller["enabled_with_ancestors"] and "qcom,msm-smmu-v2" in controller["compatible"],
            "Missing/disabled legacy SMMU controller")
    controllers_props = controller["properties"]
    base, length = provider.cells(controllers_props["reg"])
    offset = one(controllers_props, "qcom,cb-base-offset") if "qcom,cb-base-offset" in controllers_props else 0x8000
    records = []
    for contract in contracts:
        path = "/display/mdp-secure" if contract["secure_domain"] else "/display/mdp-unsecure"
        props = nodes[path]["properties"]
        require(text(props, "forge,legacy-client") == contract["client_name"] and
                text(props, "forge,legacy-context-name") == contract["context_name"], "Changed legacy display identity")
        matches = [n for n in baseline if "qcom,msm-smmu-v2-ctx" in n["compatible"] and
                   "label" in n["properties"] and text(n["properties"], "label") == contract["context_name"]]
        require(len(matches) == 1 and matches[0]["enabled_with_ancestors"], "Missing/ambiguous/disabled legacy context")
        context = matches[0]
        source = context["properties"]
        require(context["path"].rsplit("/", 1)[0] == controller_path and
                text(props, "forge,baseline-context") == context["path"], "Changed display context edge")
        for prop in ("reg", "interrupts"):
            require(provider.cells(props["forge,legacy-" + prop]) == provider.cells(source[prop]),
                    "Changed measured display context metadata")
        ctx_base, ctx_size = provider.cells(source["reg"])
        relative = ctx_base - base - offset
        require(relative >= 0 and relative % 0x1000 == 0 and ctx_size == 0x1000 and
                ctx_base + ctx_size <= base + length, "Invalid measured context aperture")
        bank = relative // 0x1000
        require(one(props, "forge,legacy-context-bank") == bank, "Changed sparse context bank")
        sids = provider.cells(source["qcom,iommu-ctx-sids"])
        masks = provider.cells(source["qcom,iommu-sid-mask"]) if "qcom,iommu-sid-mask" in source else [0] * len(sids)
        require(len(masks) == len(sids) and all(0 <= v <= 0xffff for v in sids + masks), "Invalid stream metadata")
        require(provider.cells(props["iommus"]) == [v for sid, mask in zip(sids, masks) for v in (handle, sid, mask)],
                "Changed/unresolved stream provider or SID/mask")
        for flag, expected in (("forge,secure-context", "qcom,secure-context" in source),
                               ("forge,secure-domain", contract["secure_domain"])):
            require((flag in props) == expected and not props.get(flag, b""), "Changed secure metadata")
        pool = provider.cells(props["qcom,virtual-addr-pool"])
        require(pool == contract["pool"], "Changed source-defined display IOVA pool")
        records.append({**contract, "baseline_context": context["path"], "fixture_context": path,
                        "legacy_bank_index": bank, "stream_ids": sids, "masks": masks,
                        "legacy_secure_context": "qcom,secure-context" in source})
    require(len(records) == 2 and records[0]["context_name"] != records[1]["context_name"], "Invalid display contract count")
    return {"scope": "HOST_SOURCE_AND_COMPILED_DT_CONTRACTS_ONLY_NOT_RUNTIME_BINDING_OR_HARDWARE_PASS",
            "dynamic_display_domains": records,
            "open": ["Native context-bank/IRQ/static-secure ownership and handoff",
                     "Real MDSS consumer virtual children, clocks and full-board DT",
                     "Actual DMA allocation/cache/SCM/display/panel and old blob ABI"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("legacy_source", type=Path)
    parser.add_argument("legacy_sizes", type=Path)
    args = parser.parse_args()
    try:
        for path, expected in ((args.baseline, BASELINE_SHA), (args.legacy_source, SOURCE_SHA), (args.legacy_sizes, SIZES_SHA)):
            require(provider.parser.sha256(path) == expected, f"Unexpected input SHA-256: {path}")
        contracts = source_contract(args.legacy_source.read_text(), args.legacy_sizes.read_text())
        result = validate(provider.parser.dt_nodes(args.baseline.read_bytes(), include_properties=True),
                          provider.parser.dt_nodes(args.fixture.read_bytes(), include_properties=True), contracts)
        result["inputs"] = {"baseline_sha256": BASELINE_SHA, "fixture_sha256": provider.parser.sha256(args.fixture),
                            "legacy_mdss_sha256": SOURCE_SHA, "legacy_sizes_sha256": SIZES_SHA,
                            "auditor_sha256": provider.parser.sha256(Path(__file__)),
                            "provider_sha256": provider.parser.sha256(provider_path),
                            "parser_sha256": provider.parser.sha256(Path(__file__).with_name("kernel-migration-audit.py"))}
        print(json.dumps(result, indent=2, sort_keys=True))
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(f"MDSS fixture rejected: {error}") from error


if __name__ == "__main__":
    main()
