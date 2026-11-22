#!/usr/bin/env python3
"""Check or add a self-contained ARM64 awk in a C5 recovery; HOST only."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import stat
import struct
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "c5_recovery_format", Path(__file__).with_name("kernel-c5-recovery.py"))
fmt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fmt)
CAPACITY = 33554432
AWK_PATHS = ("system/bin/awk", "sbin/awk")


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def static_arm64(data):
    require(len(data) >= 64 and data[:7] == b"\x7fELF\x02\x01\x01", "awk must be little-endian ELF64")
    kind, machine = struct.unpack_from("<HH", data, 16)
    require(kind == 2 and machine == 183, "awk must be an ARM64 static executable")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    ehsize, phsize, phnum = struct.unpack_from("<HHH", data, 52)
    require(ehsize == 64 and phsize == 56 and 0 < phnum < 128, "Invalid ELF program headers")
    require(phoff >= 64 and phoff + phsize * phnum <= len(data), "Truncated ELF program headers")
    executable = False
    for n in range(phnum):
        kind, flags, offset, _, _, filesz, memsz, _ = struct.unpack_from("<II6Q", data, phoff + n * phsize)
        require(kind not in (2, 3), "awk must have no dynamic loader or dynamic dependencies")
        require(offset + filesz <= len(data) and memsz >= filesz, "Invalid ELF segment extent")
        require(not (flags & 1 and flags & 2), "Writable executable ELF segment")
        executable |= kind == 1 and bool(flags & 1)
    require(executable, "Missing executable ELF load segment")
    return {"sha256": sha(data), "bytes": len(data), "machine": "AArch64", "static": True}


def unpack(data):
    require(2048 <= len(data) <= CAPACITY and data[:8] == b"ANDROID!", "Invalid C5 recovery image")
    fields = struct.unpack_from("<10I", data, 8)
    ks, ka, rs, ra, ss, sa, ta, page, ds, _ = fields
    require((ka, ra, ss, sa, ta, page) == (0x80008000, 0x82000000, 0, 0x80f00000, 0x81e00000, 2048),
            "Unexpected C5 recovery layout")
    require(ks > 0 and rs > 0 and ds > 0, "Missing recovery payload")
    roff = page + fmt.align(ks)
    doff = roff + fmt.align(rs)
    end = doff + fmt.align(ds)
    require(end + 16 <= len(data) and data[end:end + 16] == b"SEANDROIDENFORCE", "Invalid recovery trailer")
    require(not any(data[end + 16:]), "Nonzero data after recovery trailer")
    with gzip.GzipFile(fileobj=io.BytesIO(data[roff:roff + rs])) as stream:
        raw = stream.read(64 * 1024 * 1024 + 1)
    require(len(raw) <= 64 * 1024 * 1024, "Recovery ramdisk is too large")
    entries = fmt.cpio_read(raw)
    return {"header": data[:page], "fields": fields, "kernel": data[page:page + ks],
            "ramdisk": data[roff:roff + rs], "dt": data[doff:doff + ds], "entries": entries}


def inspect(data):
    image = unpack(data)
    matches = [(name, fields, payload) for name, fields, payload in image["entries"] if name in AWK_PATHS]
    require(matches, "Recovery lacks awk; build awk_recovery or embed it before release/use")
    tools = {}
    for name, fields, payload in matches:
        require(stat.S_ISREG(fields[1]) and fields[1] & 0o111 and fields[2:5] == [0, 0, 1],
                "awk must be a root-owned regular executable, not a symlink")
        tools[name] = static_arm64(payload)
    return {"result": "RECOVERY_STATIC_AWK_PRESENT", "image_sha256": sha(data),
            "full_partition_sha256": sha(data.ljust(CAPACITY, b"\0")), "awk": tools}


def embed(data, awk):
    static_arm64(awk)
    before = unpack(data)
    entries = before["entries"]
    existing = [n for n, _, _ in entries if n in AWK_PATHS]
    if existing:
        require(all(payload == awk for name, _, payload in entries if name in existing),
                "Existing awk differs; do not silently replace a recovery tool")
        inspect(data)
        return data, {"added_entries": [], "unchanged_existing_awk": True}
    names = {n: fields for n, fields, _ in entries}
    parents = [p for p in ("system/bin", "sbin") if p in names and stat.S_ISDIR(names[p][1])]
    require(parents, "Missing recovery executable directory")
    require(entries[-1][0] == "TRAILER!!!", "Missing final CPIO trailer")
    name = parents[0] + "/awk"
    fields = [max(f[0] for _, f, _ in entries) + 1, 0o100755, 0, 0, 1, 0,
              len(awk), 0, 0, 0, 0, len(name) + 1, 0]
    changed = entries[:-1] + [(name, fields, awk)] + entries[-1:]
    packed = gzip.compress(fmt.cpio_write(changed), compresslevel=9, mtime=0)
    header = bytearray(before["header"])
    struct.pack_into("<I", header, 16, len(packed))
    identifier = hashlib.sha1()
    for payload in (before["kernel"], packed, b"", before["dt"]):
        identifier.update(payload)
        identifier.update(struct.pack("<I", len(payload)))
    header[576:608] = identifier.digest() + b"\0" * 12
    result = bytes(header)
    for payload in (before["kernel"], packed, before["dt"]):
        result += payload + b"\0" * (fmt.align(len(payload)) - len(payload))
    result += b"SEANDROIDENFORCE"
    require(len(result) <= CAPACITY, "Recovery exceeds its partition capacity")
    after = unpack(result)
    require(after["entries"] == changed, "Recovery CPIO metadata changed")
    require(after["kernel"] == before["kernel"] and after["dt"] == before["dt"], "Kernel or DT changed")
    permitted = set(range(16, 20)) | set(range(576, 608))
    require(all(a == b or n in permitted for n, (a, b) in enumerate(zip(before["header"], after["header"]))),
            "Unrelated boot header, addresses or command line changed")
    inspect(result)
    return result, {"added_entries": [name], "unchanged_original_entries": len(entries),
                    "kernel_sha256": sha(before["kernel"]), "DT_sha256": sha(before["dt"]),
                    "original_header_preserved_except_ramdisk_size_and_payload_ID": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    check = sub.add_parser("check")
    check.add_argument("--image", required=True, type=Path)
    add = sub.add_parser("embed")
    add.add_argument("--image", required=True, type=Path)
    add.add_argument("--awk", required=True, type=Path)
    add.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        original = args.image.read_bytes()
        if args.action == "check":
            report = inspect(original)
        else:
            require(not args.output.exists() and not args.output.is_symlink(), "Output must be a new path")
            artifact, delta = embed(original, args.awk.read_bytes())
            report = dict(inspect(artifact), source_image_sha256=sha(original), delta=delta)
            with args.output.open("xb") as stream:
                stream.write(artifact)
        print(json.dumps(report, indent=2))
    except (ValueError, OSError, EOFError) as ex:
        parser.exit(1, str(ex) + "\n")


if __name__ == "__main__":
    main()
