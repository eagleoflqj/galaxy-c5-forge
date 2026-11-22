#!/usr/bin/env python3
"""Build/audit a complete frozen C5 DT with native MPM topology, not a boot gate."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path

BASE_DTB_SHA = "83e02661e33cc35a25c6fee66abb0e2f492e2dc854b79a396a8eb90817869713"
BASE_DTS_SHA = "f00c736c1321e8b4386e45a31bef12093eb570d9765278cee17dca65814abfc8"
DTC_SHA = "d386e2ba413335cdd633f0da1344f19554d74e7f9418662de06bde2a6b2fa434"
GIC = "/soc/interrupt-controller@b000000"
MPM = "/soc/qcom,mpm@601d0"
GCC = "/soc/qcom,gcc@1800000"
TLMM = "/soc/pinctrl@1000000"
GPIO = TLMM + "/gp/msm_gpio"
WAKE = "/wake-gpio"
XO = "/mpm-xo"
GPU = "/soc/qcom,kgsl-3d0@1c00000"
GPU_WRAPPER = "/soc/kgsl-iommu@1f00000"
PON = "/soc/qcom,spmi@200f000/qcom,pm8950@0/qcom,power-on@800"
PON_PROPERTY = "samsung,c5lte-restart-reason"
SOC_PROPERTY = "samsung,c5lte-legacy-soc-name"
SOC_SOURCE_PINS = {
    "drivers/of/platform.c": "b97f98e203ebe990fb0beb4b2f7742912cb1b7ef34c30fe3edb73ca2267831c4",
    "drivers/of/device.c": "50b164c949d9eb65ba29130f99057a71a56b11d320220119f93512fcd65d64e5",
    "drivers/base/core.c": "7c095d1e9c2ea429e6098a10f00ac822bf939aa1dc1ade72c4118b60e2e49f09",
    "drivers/base/platform.c": "04ca20908225428f5444329d248f5df2709b544ec44494a8b9342cbf3bc5a21b",
}
PON_SOURCE_PINS = {
    "drivers/input/misc/qpnp-power-on.c": "d035ec1f6a57a3ec8495f690e47b041f130b62b6b67192b548a7e714bc303402",
    "drivers/power/reset/msm-poweroff.c": "b3a90f25481c44a631d0d294066ea38192d0757eb8faebd028c927e356e7d6f7",
    "include/linux/input/qpnp-power-on.h": "d301dc735f563bffefc1fe05722d7568949b763803eace4fee5e2e51444164a5",
}
GPU_SOURCE_PINS = {
    "drivers/gpu/msm/kgsl_iommu.c": "c3b9d5e11c84e61215fe4dbba1f1b47c20282b5f06838aa77eee36518e5c5e78",
    "drivers/gpu/msm/kgsl_iommu.h": "676b88e5af2b06647cc960e371cb9b046abe17402afe42f95be7ac644ed202e8",
    "drivers/gpu/msm/adreno.c": "6b9b0545c149776516a37a053fe1fe2882ac227d4c0c7ad77caf57573be46751",
    "drivers/gpu/msm/adreno.h": "c8663cf75369b3be1c28bd079825a736a1e3c8ceff88748078fcd4b543263b3e",
    "drivers/gpu/msm/adreno_a4xx.c": "6cffa78de0c7d83587c43fecdfa1ae5e5b44b963377d52d4a1b4b50550e078b2",
}
MEANING = "FULL_C5_DT_IRQ_SMMU_MDSS_KGSL_RESOURCES_ONLY_NOT_BOOTABLE_NOT_HARDWARE_ACCEPTANCE"
spec = importlib.util.spec_from_file_location("migration", Path(__file__).with_name("kernel-migration-audit.py"))
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)
spec = importlib.util.spec_from_file_location("mdss_contract", Path(__file__).with_name("kernel-c5-mdss-dt-audit.py"))
mdss_contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mdss_contract)
MDSS_POOLS = [("mdp_0", "mdp_ns", [0x20000, 0x3ffe0000], False),
              ("mdp_1", "mdp_secure", [0x40000000, 0x40000000], True)]


def reviewed_mdss_contracts():
    return [{"context_name": ctx, "client_name": client, "pool": pool,
             "secure_domain": secure} for ctx, client, pool, secure in MDSS_POOLS]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cells(*values):
    return struct.pack(f">{len(values)}I", *values)


def strings(*values):
    return b"".join(value.encode("ascii") + b"\0" for value in values)


def string_values(value):
    if not value:
        return []
    require(value.endswith(b"\0"), "Unterminated DT string")
    return value[:-1].decode("ascii").split("\0")


def tree(path):
    nodes = migration.dt_nodes(path.read_bytes(), include_properties=True)
    result = {node["path"]: node["properties"] for node in nodes}
    require(len(nodes) == len(result), "Duplicate node path")
    return result


def layout(path):
    """Retain boot CPU and every reserve-map tuple, not serialized offsets."""
    blob = path.read_bytes()
    require(len(blob) >= 40, "Truncated FDT header")
    magic, total, structure, stringoff, reserved, version, compatible, cpu, stringlen, structlen = struct.unpack_from(
        ">10I", blob)
    require(magic == 0xd00dfeed and version == 17 and compatible <= 17 and total == len(blob), "Invalid FDT envelope")
    require(40 <= structure <= total and structlen <= total - structure and
            40 <= stringoff <= total and stringlen <= total - stringoff, "Invalid FDT block bounds")
    require(reserved >= 40 and reserved % 8 == 0, "Invalid reserve-map offset")
    tuples = []
    limit = min(structure, stringoff)
    while reserved + 16 <= limit:
        address, size = struct.unpack_from(">QQ", blob, reserved)
        reserved += 16
        if address == size == 0:
            return {"boot_cpuid_phys": cpu, "memory_reservations": tuples}
        tuples.append([address, size])
    raise ValueError("Unterminated reserve map")


def handle(props):
    value = props.get("phandle", props.get("linux,phandle"))
    require(value is not None and len(value) == 4, "Invalid or absent phandle")
    if "linux,phandle" in props:
        require(value == props["linux,phandle"], "Inconsistent phandle aliases")
    number, = struct.unpack(">I", value)
    require(0 < number < 0xffffffff, "Reserved phandle")
    return number


def handles(nodes):
    found = {}
    for path, props in nodes.items():
        if "phandle" in props or "linux,phandle" in props:
            number = handle(props)
            require(number not in found, "Duplicate phandle")
            found[number] = path
    return found


def words(value):
    require(len(value) % 4 == 0, "Incomplete interrupt-map cell")
    return list(struct.unpack(f">{len(value) // 4}I", value))


def one(props, name, default=None):
    if name not in props and default is not None:
        return default
    values = words(props[name])
    require(len(values) == 1, f"Expected single cell: {name}")
    return values[0]


def interrupt_map(nodes, path):
    """Decode all entries using actual local/parent cell widths, never search integers."""
    props = nodes[path]
    addrsize = one(props, "#address-cells")
    intsize = one(props, "#interrupt-cells")
    require(0 < intsize <= 16 and addrsize + intsize <= 16, "Invalid nexus cell widths")
    width = addrsize + intsize
    mask = words(props.get("interrupt-map-mask", cells(*([0xffffffff] * width))))
    require(len(mask) == width, "Invalid interrupt-map mask width")
    raw = words(props["interrupt-map"])
    require(raw, "Empty interrupt-map")
    index = handles(nodes)
    entries, keys, offset = [], set(), 0
    while offset < len(raw):
        require(len(raw) - offset >= width + 1, "Truncated interrupt-map key/parent")
        child = raw[offset:offset + width]
        key = tuple(value & bits for value, bits in zip(child, mask))
        require(key not in keys, "Ambiguous masked interrupt-map key")
        keys.add(key)
        parent_offset = offset + width
        parent = index.get(raw[parent_offset])
        require(parent is not None, "Unresolved interrupt-map parent")
        parentaddr = one(nodes[parent], "#address-cells", 0)
        parentint = one(nodes[parent], "#interrupt-cells")
        require(0 < parentint <= 16 and parentaddr + parentint <= 16, "Invalid map parent cell widths")
        end = parent_offset + 1 + parentaddr + parentint
        require(end <= len(raw), "Truncated interrupt-map parent tuple")
        entries.append({"child": child, "parent": parent, "parent_offset": parent_offset,
                        "parent_addr": raw[parent_offset + 1:parent_offset + 1 + parentaddr],
                        "specifier": raw[parent_offset + 1 + parentaddr:end]})
        offset = end
    return entries


def smmu_plan(base):
    """Assemble native controller resources from measured legacy context nodes."""
    controllers = sorted(path for path, props in base.items()
                         if "qcom,msm-smmu-v2" in string_values(props.get("compatible", b"")))
    edits, records, contexts = {}, [], {}
    # Native msm_tz_smmu.h enum + device_id_mappings: APPS=17, GPU=18.
    secure_names = {17: "APPS", 18: "GPU"}
    for path in controllers:
        props = base[path]
        reg = words(props["reg"])
        require(len(reg) == 2 and reg[1] > 0 and reg[0] % 0x1000 == 0 and reg[1] % 0x2000 == 0,
                "Invalid measured SMMU aperture")
        address, length = reg
        offset = one(props, "qcom,cb-base-offset", 0x8000)
        require(offset == length // 2 and offset % 0x1000 == 0, "Native SMMU CB aperture differs")
        secure_id = one(props, "qcom,iommu-secure-id")
        require(secure_id in secure_names, "Unreviewed SMMU secure device identity")
        require(irq_parent(base, path) == GIC, "SMMU global IRQ not measured GIC")
        global_irqs = words(props["interrupts"])
        require(len(global_irqs) == 6, "Expected two measured SMMU global IRQs")
        global_names = string_values(props["interrupt-names"])
        require(len(global_names) == 2, "Expected two measured global IRQ names")
        flattened, names, banks, bank_records = list(global_irqs), list(global_names), [], []
        for child, source in sorted(base.items()):
            if "qcom,msm-smmu-v2-ctx" not in string_values(source.get("compatible", b"")):
                continue
            if child.rsplit("/", 1)[0] != path:
                continue
            cbreg = words(source["reg"])
            require(len(cbreg) == 2, "Invalid context reg tuple")
            cbaddr, cbsize = cbreg
            relative = cbaddr - address - offset
            require(relative >= 0 and relative % 0x1000 == 0 and cbsize == 0x1000 and
                    cbaddr + cbsize <= address + length, "Context outside measured SMMU aperture")
            bank = relative // 0x1000
            require(bank not in banks and bank < 128, "Duplicate/out-of-range measured context bank")
            require(irq_parent(base, child) == GIC, "Context IRQ not measured GIC")
            irq = words(source["interrupts"])
            require(len(irq) in (3, 6) and (len(irq) == 3 or irq[:3] == irq[3:]),
                    "Context must have one IRQ or identical legacy duplicate")
            require(irq[0] == 0 and irq[1] < 988 and irq[2] & 0xf in (0, 1, 2, 4, 8),
                    "Context IRQ outside native MPM geometry")
            labels = string_values(source["label"])
            require(len(labels) == 1 and labels[0] not in contexts, "Duplicate context identity")
            ids = words(source["qcom,iommu-ctx-sids"])
            masks = words(source.get("qcom,iommu-sid-mask", cells(*([0] * len(ids)))))
            require(ids and len(ids) == len(masks) and all(sid <= 0xffff and mask <= 0xffff
                                                        for sid, mask in zip(ids, masks)), "Invalid context SID/mask")
            banks.append(bank)
            names.append(labels[0])
            flattened.extend(irq[:3])
            record = {"path": child, "bank": bank, "irq_index": len(banks) - 1,
                      "irq": irq[:3], "stream_ids": ids, "masks": masks,
                      "secure_metadata": "qcom,secure-context" in source}
            contexts[labels[0]] = {**record, "controller": path}
            bank_records.append(record)
        require(banks, "Native controller has no measured contexts")
        edits[path] = {"compatible": strings("qcom,smmu-v2"), "#iommu-cells": cells(2),
                       "#global-interrupts": cells(2), "qcom,tz-device-id": strings(secure_names[secure_id]),
                       "interrupts": cells(*flattened), "interrupt-names": strings(*names),
                       "qcom,context-bank-irq-map": cells(*banks)}
        records.append({"path": path, "secure_id": secure_id, "secure_name": secure_names[secure_id],
                        "reg": reg, "cb_offset": offset, "global_irqs": global_irqs,
                        "context_irqs": bank_records,
                        "aperture_capacity_NOT_verified_hardware_count": (length - offset) // 0x1000})
    masters = []
    index = handles(base)
    for path, props in sorted(base.items()):
        if "iommus" not in props:
            continue
        raw = words(props["iommus"])
        require(raw and len(raw) % 2 == 0, "Invalid measured legacy IOMMU tuples")
        labels = string_values(props.get("label", b""))
        require(len(labels) == 1 and labels[0] in contexts, "Unreviewed legacy IOMMU consumer identity")
        context = contexts[labels[0]]
        require(all(index.get(raw[i]) == context["controller"] for i in range(0, len(raw), 2)) and
                raw[1::2] == context["stream_ids"], "Consumer differs from measured context streams")
        converted = []
        for handle_value, sid, mask in zip(raw[::2], raw[1::2], context["masks"]):
            converted.extend((handle_value, sid, mask))
        edits[path] = {"iommus": cells(*converted), "qcom,fixed-context-bank": cells(context["bank"])}
        masters.append({"path": path, "context": context["path"], "controller": context["controller"],
                        "bank": context["bank"], "stream_ids": context["stream_ids"], "masks": context["masks"]})
    return edits, {"controllers": records, "existing_iommu_masters": masters,
                   "scope": "NATIVE_CONTROLLER_RESOURCE_DT_ONLY_NOT_FIRMWARE_OR_CONSUMER_DRIVER_ACCEPTANCE"}


def pon_plan(base):
    """Select only the measured C5 system-reset bootloader ABI; no address edit."""
    if PON not in base:
        return {}
    props = base[PON]
    require(props.get("compatible") == strings("qcom,qpnp-power-on") and
            props.get("reg") == cells(0x800, 0x100), "Unreviewed C5 PON identity")
    require(props.get("qcom,system-reset") == b"" and
            props.get("qcom,store-hard-reset-reason") == b"", "Missing C5 reset/store policy")
    require(PON_PROPERTY not in props, "C5 reason binding already present in baseline")
    for path in (PON, PON.rsplit("/", 1)[0], "/soc/qcom,spmi@200f000", "/soc", "/"):
        require(base[path].get("status", strings("okay")) in (strings("okay"), strings("ok")), "Disabled C5 PON ancestor")
    return {PON: {PON_PROPERTY: b""}}


def soc_name_plan(base):
    """Preserve the measured root bus name, not unrelated no-reg counters."""
    if PON not in base:
        return {}
    props = base["/soc"]
    require(props.get("compatible") == strings("simple-bus") and "reg" not in props,
            "Unreviewed C5 root SoC bus")
    require(SOC_PROPERTY not in props, "C5 namespace binding already present")
    for path in ("/", "/soc"):
        require(base[path].get("status", strings("okay")) in (strings("okay"), strings("ok")), "Disabled C5 SoC ancestor")
    return {"/soc": {SOC_PROPERTY: b""}}


def plan(base):
    """Changes are derived from measured handles; no hardware addresses added."""
    inventory = handles(base)
    require(WAKE not in base and XO not in base, "Virtual-node collision")
    gic = handle(base[GIC])
    require(base["/"]["interrupt-parent"] == cells(gic), "Unexpected root IRQ parent")
    require(base[MPM]["compatible"] == strings("qcom,mpm-v2"), "Unexpected legacy MPM")
    require(base[MPM]["qcom,gic-parent"] == cells(gic), "Legacy MPM GIC mismatch")
    require(base[MPM]["qcom,gpio-parent"] == cells(handle(base[GPIO])), "Legacy GPIO mismatch")
    require(base[MPM]["qcom,ipc-bit-offset"] == cells(1), "Unexpected native IPC bit")
    require(base[TLMM + "/gp"]["qcom,num-pins"] == cells(134), "Unexpected TLMM pin count")
    require(base[GPIO]["num_irqs"] == cells(134), "Unexpected GPIO IRQ count")
    require(base[MPM]["clocks"][:4] == cells(handle(base[GCC])) and len(base[MPM]["clocks"]) == 8,
            "XO provider must be measured GCC")
    top = max(inventory)
    require(top + (3 if GPU in base else 2) < 0xffffffff, "No fresh phandle namespace")
    mpm, wake = top + 1, top + 2
    edits = {}
    for path, props in base.items():
        if props.get("interrupt-parent") == cells(gic) and path not in (GIC, MPM):
            edits[path] = {"interrupt-parent": cells(mpm)}
    # irqchip init recognizes a self-parent as NULL. Otherwise root inheritance
    # would incorrectly make GIC wait on its own MPM child.
    edits[GIC] = {"interrupt-parent": cells(gic)}
    edits.setdefault(TLMM, {})["interrupt-parent"] = cells(mpm)
    edits[GPIO] = {"interrupt-parent": cells(wake)}
    edits[MPM] = {"compatible": strings("qcom,mpm-gic-c5lte", "qcom,mpm-gic"),
                  "interrupt-parent": cells(gic), "interrupt-controller": b"",
                  "#interrupt-cells": cells(3), "#address-cells": cells(0),
                  "qcom,num-mpm-irqs": cells(64), "phandle": cells(mpm)}
    added = {
        WAKE: {"compatible": strings("qcom,mpm-gpio-c5lte", "qcom,mpm-gpio"),
               "interrupt-parent": cells(mpm), "interrupt-controller": b"",
               "#address-cells": cells(0), "#interrupt-cells": cells(2), "phandle": cells(wake)},
        XO: {"compatible": strings("qcom,mpm-c5lte-xo"),
             "qcom,mpm-gic-domain": cells(mpm), "qcom,mpm-gpio-domain": cells(wake),
             "clocks": base[MPM]["clocks"], "clock-names": base[MPM]["clock-names"]},
    }
    for path, props in base.items():
        if "interrupt-map" not in props:
            continue
        raw = words(props["interrupt-map"])
        for entry in interrupt_map(base, path):
            if entry["parent"] == GIC:
                require(not entry["parent_addr"] and len(entry["specifier"]) == 3,
                        "GIC map geometry differs from native MPM")
                raw[entry["parent_offset"]] = mpm
        if cells(*raw) != props["interrupt-map"]:
            edits.setdefault(path, {})["interrupt-map"] = cells(*raw)
    smmu_edits, _ = smmu_plan(base)
    for path, props in smmu_edits.items():
        edits.setdefault(path, {}).update(props)
    mdss_nodes, _ = mdss_plan(base)
    added.update(mdss_nodes)
    gpu_edits, gpu_nodes, _ = kgsl_plan(base)
    for path, props in gpu_edits.items():
        edits.setdefault(path, {}).update(props)
    added.update(gpu_nodes)
    for path, props in pon_plan(base).items():
        edits.setdefault(path, {}).update(props)
    for path, props in soc_name_plan(base).items():
        edits.setdefault(path, {}).update(props)
    return edits, added


def kgsl_plan(base):
    """Preserve actual old USER/SECURE consumption; private legacy metadata stays."""
    if GPU not in base:
        return {}, {}, None
    gpu = base[GPU]
    require(one(gpu, "qcom,chipid") == 0x04000510, "Unreviewed C5 GPU register generation")
    reg = words(gpu["reg"])
    require(len(reg) == 4 and reg[0] == 0x1c00000, "Unreviewed GPU register geometry")
    require(GPU_WRAPPER not in base and not any(path.startswith(GPU_WRAPPER + "/") for path in base),
            "GPU wrapper collision")
    controller = handles(base).get(one(gpu, "iommu"))
    require(controller is not None, "Unresolved legacy GPU IOMMU")
    _, resources = smmu_plan(base)
    matches = [record for record in resources["controllers"] if record["path"] == controller]
    require(len(matches) == 1 and matches[0]["secure_id"] == 18, "GPU IOMMU is not measured GPU controller")
    record = matches[0]
    require(record["reg"] == [0x1f00000, 0x10000] and record["cb_offset"] == 0x8000,
            "GPU CB aperture differs from reviewed A405v2/native base")
    props = base[controller]
    names = string_values(props["clock-names"])
    require(names == ["iface_clk", "core_clk"] and len(words(props["clocks"])) == 4,
            "Unreviewed GPU wrapper clocks")
    for provider in words(props["clocks"])[::2]:
        require(handles(base).get(provider) == GCC and one(base[GCC], "#clock-cells") == 1,
                "GPU clock is not measured GCC provider")
    for path in (GPU, controller):
        require(base[path].get("status", strings("okay")) in (strings("okay"), strings("ok")),
                "Disabled GPU/controller")
    wrapper_handle = max(handles(base)) + 3
    offset = record["reg"][0] - reg[0]
    added = {GPU_WRAPPER: {
        "compatible": strings("qcom,kgsl-smmu-v2"), "reg": props["reg"],
        "#address-cells": cells(1), "#size-cells": cells(1), "phandle": cells(wrapper_handle),
        "qcom,protect": cells(offset, 4 * (1 << 10)),
        "clocks": props["clocks"], "clock-names": props["clock-names"]}}
    for feature in ("qcom,global_pt", "qcom,retention", "qcom,hyp_secure_alloc", "qcom,force-32bit"):
        if feature in props:
            require(not props[feature], "Nonempty legacy GPU feature flag")
            added[GPU_WRAPPER][feature] = b""
    children, private = [], []
    for context in record["context_irqs"]:
        label, = string_values(base[context["path"]]["label"])
        if label == "gfx3d_priv":
            private.append(context)
            continue
        require(label in ("gfx3d_user", "gfx3d_secure"), "Unreviewed GPU context name")
        require(base[context["path"]].get("status", strings("okay")) in (strings("okay"), strings("ok")),
                "Disabled GPU context")
        path = GPU_WRAPPER + "/" + label
        added[path] = {"compatible": strings("qcom,smmu-kgsl-cb"),
                       "iommus": cells(*[value for sid, mask in zip(context["stream_ids"], context["masks"])
                                         for value in (handle(props), sid, mask)]),
                       "qcom,fixed-context-bank": cells(context["bank"])}
        if label == "gfx3d_user":
            require(context["bank"] == 0, "GPU user GPU-access offset not reviewed for nonzero bank")
            added[path]["qcom,gpu-offset"] = cells(offset)
        children.append({**context, "label": label, "wrapper_child": path})
    require(sorted(item["label"] for item in children) == ["gfx3d_secure", "gfx3d_user"] and len(private) == 1,
            "Missing/duplicate reviewed GPU contexts")
    return {GPU: {"iommu": cells(wrapper_handle)}}, added, {
        "wrapper": GPU_WRAPPER, "controller": controller, "reg": record["reg"],
        "gpu_access_offset": offset, "protection_bytes": [offset, 4 * (1 << 10)],
        "consumed_contexts": children, "legacy_private_preserved_NOT_consumed_by_old_KGSL": private,
        "scope": "HOST_WRAPPER_PROBE_ONLY_NOT_GPU_DMA_OR_FIRMWARE_ACCEPTANCE"}


def mdss_plan(base):
    """Native MDSS children; pools are pinned legacy-source contracts, not defaults."""
    parent = mdss_contract.CONSUMER
    if parent not in base:
        return {}, []
    require("qcom,mdss_mdp" in string_values(base[parent].get("compatible", b"")),
            "Unexpected measured MDSS consumer")
    require(base[parent].get("status", strings("okay")) in (strings("okay"), strings("ok")),
            "Disabled MDSS consumer")
    _, smmu = smmu_plan(base)
    added, records = {}, []
    for contract in reviewed_mdss_contracts():
        matches = [(controller, context) for controller in smmu["controllers"]
                   for context in controller["context_irqs"]
                   if string_values(base[context["path"]]["label"]) == [contract["context_name"]]]
        require(len(matches) == 1, "Missing/ambiguous measured MDSS context")
        controller, context = matches[0]
        require(controller["secure_id"] == 17, "MDSS context not on measured APPS controller")
        for path in (controller["path"], context["path"]):
            require(base[path].get("status", strings("okay")) in (strings("okay"), strings("ok")),
                    "Disabled MDSS SMMU resource")
        props = base[controller["path"]]
        clock_names = string_values(props["clock-names"])
        require(clock_names == ["iface_clk", "core_clk"] and len(words(props["clocks"])) == 4,
                "Unreviewed measured MDSS clock geometry")
        for provider, selector in zip(words(props["clocks"])[::2], words(props["clocks"])[1::2]):
            require(handles(base).get(provider) == GCC and one(base[GCC], "#clock-cells") == 1,
                    "MDSS clock not measured single-cell GCC provider")
        suffix = "sec" if contract["secure_domain"] else "unsec"
        path = parent + "/qcom,smmu_mdp_" + suffix + "_cb"
        require(path not in base, "MDSS child collision")
        streams = [value for sid, mask in zip(context["stream_ids"], context["masks"])
                   for value in (handle(props), sid, mask)]
        added[path] = {"compatible": strings("qcom,smmu_mdp_" + suffix),
                       "iommus": cells(*streams), "qcom,fixed-context-bank": cells(context["bank"]),
                       "qcom,virtual-addr-pool": cells(*contract["pool"]),
                       "clocks": props["clocks"], "clock-names": props["clock-names"]}
        records.append({**contract, "path": path, "context": context["path"],
                        "controller": controller["path"], "bank": context["bank"],
                        "stream_ids": context["stream_ids"], "masks": context["masks"],
                        "register_secure_metadata_NOT_buffer_policy": context["secure_metadata"],
                        "clock_cells": words(props["clocks"]), "clock_names": clock_names})
    return added, records


def property_dts(name, value):
    if not value:
        return f"\t{name};"
    if name in ("compatible", "clock-names", "interrupt-names", "qcom,tz-device-id"):
        decoded = value[:-1].decode("ascii").split("\0")
        return f"\t{name} = " + ", ".join(json.dumps(s) for s in decoded) + ";"
    require(len(value) % 4 == 0, "Malformed cell property")
    words = struct.unpack(f">{len(value) // 4}I", value)
    return f"\t{name} = <" + " ".join(hex(word) for word in words) + ">;"


def overlay(base):
    edits, added = plan(base)
    chunks = ["/* Native C5 MPM integration delta. NOT a completed 4.9 board port. */"]
    for path, props in sorted(edits.items()):
        chunks.append(f"&{{{path}}} {{\n" + "\n".join(property_dts(k, v) for k, v in sorted(props.items())) + "\n};")
    for path, props in sorted(added.items()):
        parent, name = path.rsplit("/", 1)
        chunks.append(f"&{{{parent or '/'}}} {{\n\t" + name + " {\n" + "\n".join("\t" + property_dts(k, v) for k, v in sorted(props.items())) + "\n\t};\n};")
    return "\n\n".join(chunks) + "\n"


def irq_parent(nodes, path):
    """Mirror normal of_irq_find_parent, including nearest ancestor cell owner."""
    index = handles(nodes)
    visited = set()
    while path is not None:
        require(path not in visited, "IRQ parent cycle without cell owner")
        visited.add(path)
        props = nodes[path]
        if "interrupt-parent" in props:
            require(len(props["interrupt-parent"]) == 4, "Invalid IRQ parent cell")
            number, = struct.unpack(">I", props["interrupt-parent"])
            require(number in index, "Unresolved IRQ parent")
            parent = index[number]
        else:
            parent = path.rsplit("/", 1)[0] or "/" if path != "/" else None
        if parent is not None and "#interrupt-cells" in nodes[parent]:
            return parent
        path = parent
    return None


def audit(base, candidate):
    edits, added = plan(base)
    require(set(candidate) == set(base) | set(added), "Added or removed board node outside MPM delta")
    differences = []
    for path, original in sorted(base.items()):
        expected = dict(original)
        expected.update(edits.get(path, {}))
        require(candidate[path] == expected, f"Unexpected board property delta: {path}")
        for key in sorted(expected):
            if original.get(key) != expected[key]:
                differences.append({"path": path, "property": key, "before_hex": original.get(key, b"").hex(),
                                    "after_hex": expected[key].hex(), "new_property": key not in original})
    for path, props in added.items():
        require(candidate[path] == props, f"Unexpected virtual node: {path}")
    index = handles(candidate)
    require(len(index) == len(handles(base)) + (3 if GPU in base else 2), "Unexpected phandle count")
    # Independently walk every inherited interrupt parent. Only original GIC
    # consumers should be rerouted; GPIO/SPMI and all other parents stay exact.
    routed = []
    for path, props in base.items():
        if "interrupts" not in props or path in (GIC, MPM):
            continue
        before = irq_parent(base, path)
        after = irq_parent(candidate, path)
        target = MPM if before == GIC else before
        require(after == target, f"Incorrect inherited IRQ routing: {path}")
        if before != after:
            require(len(props["interrupts"]) % 12 == 0, f"Not a measured three-cell GIC consumer: {path}")
            words = struct.unpack(f">{len(props['interrupts']) // 4}I", props["interrupts"])
            for kind, number, flags in zip(words[::3], words[1::3], words[2::3]):
                require(kind in (0, 1) and number < (988 if kind == 0 else 16) and flags & 0xf in (0, 1, 2, 4, 8),
                        f"GIC consumer outside native allocation contract: {path}")
            routed.append(path)
    require(candidate[GIC]["interrupt-parent"] == cells(handle(candidate[GIC])), "GIC init dependency cycle")
    require(candidate[MPM]["interrupt-parent"] == cells(handle(candidate[GIC])), "MPM wake IRQ not direct GIC")
    nexuses = []
    for path, props in base.items():
        if "interrupt-map" not in props:
            continue
        before = interrupt_map(base, path)
        after = interrupt_map(candidate, path)
        require(len(before) == len(after), "Changed map entry count")
        for old, new in zip(before, after):
            require(new["child"] == old["child"] and new["parent_addr"] == old["parent_addr"] and
                    new["specifier"] == old["specifier"] and
                    new["parent"] == (MPM if old["parent"] == GIC else old["parent"]),
                    f"Changed map tuple or routing: {path}")
        nexuses.append({"path": path, "entries": after})
    _, smmu = smmu_plan(base)
    return {"meaning": MEANING, "baseline_node_count": len(base), "candidate_node_count": len(candidate),
            "rerouted_gic_consumers": routed, "property_changes": differences, "added_nodes": sorted(added),
            "migrated_interrupt_nexuses": nexuses,
            "native_smmu_controller_resources": smmu,
            "native_mdss_children": mdss_plan(base)[1],
            "native_kgsl_wrapper": kgsl_plan(base)[2],
            "c5_pmic_restart_reason_opt_in": {"node": PON, "property": PON_PROPERTY,
                                             "scope": "HOST_SOURCE_ABI_SELECTION_NOT_HARDWARE_ACCEPTANCE"} if PON in base else None,
            "c5_soc_name_opt_in": {"node": "/soc", "property": SOC_PROPERTY, "name": "soc.0",
                                    "scope": "HOST_NAME_SELECTION_NOT_SYSFS_OR_BLOCK_DEVICE_ACCEPTANCE"} if PON in base else None,
            "preserved_interrupt_nexuses_pending_review": sorted(path for path, props in base.items() if "interrupts-extended" in props),
            "unmodified_board_properties_verified": True, "new_kernel_hardware": "NOT TESTED",
            "pending": ["SMMU firmware ownership/reset/fault permissions and remaining consumer conversion",
                        "KGSL register/secure-domain/DMA/ABI behavior; MDSS DMA/secure handoff", "remaining driver/provider integration",
                        "board kernel/config and recovery image", "native-AIO/no-replug and hardware acceptance",
                        "post-2023 security applicability"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build")
    build.add_argument("baseline", type=Path)
    build.add_argument("dtc", type=Path)
    build.add_argument("output", type=Path)
    build.add_argument("--native-source", type=Path)
    check = sub.add_parser("audit")
    check.add_argument("baseline_dtb", type=Path)
    check.add_argument("candidate_dtb", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "audit":
            require(sha(args.baseline_dtb) == BASE_DTB_SHA, "Unexpected baseline DTB")
            require(layout(args.baseline_dtb) == layout(args.candidate_dtb), "Changed boot CPU or memory reservations")
            result = audit(tree(args.baseline_dtb), tree(args.candidate_dtb))
            print(json.dumps(result, indent=2, sort_keys=True))
            return
        require(all(path.is_absolute() for path in (args.baseline, args.dtc, args.output)), "Absolute paths required")
        baseline = args.baseline.resolve(strict=True)
        dtc = args.dtc.resolve(strict=True)
        output = args.output.absolute()
        require(not output.exists() and not output.is_symlink(), "Refusing existing output")
        project = Path(__file__).resolve().parent.parent
        for root in (project, baseline, dtc.parent):
            require(not output.resolve().is_relative_to(root), "Output overlaps protected input/workspace")
        require(not any((parent / ".repo").exists() for parent in output.resolve().parents), "Output inside Android checkout")
        dts = baseline / "source/arch/arm/boot/dts/samsung/msm8952/msm8952-sec-c5lte-chn-hk-r09.dts"
        dtb = baseline / "build/arch/arm64/boot/dts/msm8952-sec-c5lte-chn-hk-r09.dtb"
        require(sha(dts) == BASE_DTS_SHA and sha(dtb) == BASE_DTB_SHA, "Unexpected frozen C5 board input")
        require(sha(dtc) == DTC_SHA, "Unexpected pinned native 4.9 DTC")
        mdss_source = baseline / "source/drivers/video/msm/mdss/mdss_mdp.c"
        sizes = baseline / "source/include/linux/sizes.h"
        require(sha(mdss_source) == mdss_contract.SOURCE_SHA and sha(sizes) == mdss_contract.SIZES_SHA,
                "Unexpected legacy MDSS source contract input")
        require(mdss_contract.source_contract(mdss_source.read_text(), sizes.read_text()) == reviewed_mdss_contracts(),
                "Changed reviewed legacy MDSS pools/security policy")
        gpu_sources = [baseline / "source" / path for path in GPU_SOURCE_PINS]
        for path, digest in GPU_SOURCE_PINS.items():
            require(sha(baseline / "source" / path) == digest, f"Unexpected legacy GPU contract: {path}")
        legacy = gpu_sources[0].read_text()
        require(re.search(r'else if \(!strcmp\("gfx3d_priv",\s*data->iommu_ctxs\[i\].iommu_ctx_name\)\) \{\s*continue;', legacy),
                "Old KGSL does not skip private context")
        require("iommuv2_regs.range = 10;" in legacy and "data->regstart - mmu->device->reg_phys" in legacy,
                "Changed legacy GPU protection/address contract")
        base = tree(dtb)
        require(PON in base, "Missing measured C5 PON node")
        require(args.native_source is not None and args.native_source.is_absolute(), "Absolute --native-source required for C5 PMIC ABI selection")
        for path, digest in (PON_SOURCE_PINS | SOC_SOURCE_PINS).items():
            require(sha(args.native_source / path) == digest, f"Unexpected native C5 PMIC source: {path}")
        delta = overlay(base)
        os.umask(0o077)
        output.mkdir(parents=True, mode=0o700)
        started = datetime.now(timezone.utc).isoformat()
        (output / "kernel-c5-board-dt.py").write_bytes(Path(__file__).read_bytes())
        (output / "kernel-migration-audit.py").write_bytes(Path(migration.__file__).read_bytes())
        for helper in ("kernel-c5-mdss-dt-audit.py", "kernel-c5-provider-audit.py"):
            (output / helper).write_bytes(Path(__file__).with_name(helper).read_bytes())
        (output / "mpm-delta.dtsi").write_text(delta)
        source = output / "msm8952-sec-c5lte-chn-hk-r09-mpm.dts"
        source.write_text(dts.read_text() + "\n" + delta)
        candidate = output / "msm8952-sec-c5lte-chn-hk-r09-mpm.dtb"
        command = [str(dtc), "-I", "dts", "-O", "dtb", "-b", "0", "-o", str(candidate), str(source)]
        (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
        completed = subprocess.run(command, capture_output=True, check=False)
        (output / "dtc.stdout").write_bytes(completed.stdout)
        (output / "dtc.stderr").write_bytes(completed.stderr)
        require(completed.returncode == 0, f"DTC failed: {completed.returncode}; logs retained")
        require(layout(dtb) == layout(candidate), "Changed boot CPU or memory reservations")
        result = audit(base, tree(candidate))
        result["layout"] = layout(candidate)
        result["started_utc"] = started
        result["finished_utc"] = datetime.now(timezone.utc).isoformat()
        result["dtc_returncode"] = completed.returncode
        result["dtc_warning_count"] = completed.stderr.count(b"Warning (")
        result["inputs_sha256"] = {str(path): sha(path) for path in (
            dts, dtb, dtc, mdss_source, sizes, Path(__file__).resolve(), Path(migration.__file__),
            Path(mdss_contract.__file__), Path(mdss_contract.provider.__file__))}
        result["inputs_sha256"].update({str(path): sha(path) for path in gpu_sources})
        result["inputs_sha256"].update({str(args.native_source / path): sha(args.native_source / path) for path in PON_SOURCE_PINS | SOC_SOURCE_PINS})
        result["outputs_sha256"] = {path.name: sha(path) for path in (source, candidate, output / "mpm-delta.dtsi", output / "dtc.stderr", output / "command.json")}
        (output / "audit.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print(json.dumps(result, indent=2, sort_keys=True))
    except (ValueError, OSError, KeyError, struct.error) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
