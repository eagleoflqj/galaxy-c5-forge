"""Recovery awk packaging invariants; real ARM64 execution is checked separately."""
import copy
import gzip
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "recovery_awk", Path(__file__).parents[1] / "scripts/recovery-awk.py")
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


def entry(name, payload=b"", mode=0o100644):
    return name, [1, mode, 0, 0, 1, 0, len(payload), 0, 0, 0, 0, len(name) + 1, 0], payload


def elf(segment=1, machine=183, flags=5):
    data = bytearray(128)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHHHHH", data, 16, 2, machine, 1, 120, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into("<II6Q", data, 64, segment, flags, 0, 0, 0, 128, 128, 4096)
    return bytes(data)


def image(entries):
    kernel, packed, dt = b"UNCHANGED_KERNEL", gzip.compress(r.fmt.cpio_write(entries), mtime=0), b"UNCHANGED_DT"
    header = bytearray(2048)
    header[:8] = b"ANDROID!"
    struct.pack_into("<10I", header, 8, len(kernel), 0x80008000, len(packed), 0x82000000,
                     0, 0x80f00000, 0x81e00000, 2048, len(dt), 0)
    header[64:80] = b"UNCHANGED_CMDLIN"
    result = bytes(header)
    for payload in (kernel, packed, dt):
        result += payload + b"\0" * (r.fmt.align(len(payload)) - len(payload))
    return result + b"SEANDROIDENFORCE"


class RecoveryAwkTests(unittest.TestCase):
    def setUp(self):
        self.entries = [entry("system/bin", mode=0o40755), entry("init.rc", b"original USB and mounts"),
                        entry("sbin/sh", b"../system/bin/sh", mode=0o120777), entry("TRAILER!!!")]

    def test_missing_awk_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "lacks awk"):
            r.inspect(image(self.entries))

    def test_embedding_preserves_every_existing_cpio_entry_and_payload(self):
        before = image(self.entries)
        after, report = r.embed(before, elf())
        entries = r.unpack(after)["entries"]
        self.assertEqual(entries[:-2] + entries[-1:], self.entries)
        self.assertEqual(report["added_entries"], ["system/bin/awk"])
        self.assertEqual(r.unpack(after)["kernel"], r.unpack(before)["kernel"])
        self.assertEqual(r.unpack(after)["dt"], r.unpack(before)["dt"])
        self.assertEqual(r.unpack(after)["header"][64:576], r.unpack(before)["header"][64:576])
        self.assertEqual(r.inspect(after)["awk"]["system/bin/awk"]["bytes"], 128)
        self.assertEqual(r.embed(before, elf())[0], after)

    def test_legacy_sbin_ramdisk_supported(self):
        entries = copy.deepcopy(self.entries)
        entries[0] = entry("sbin", mode=0o40755)
        after, _ = r.embed(image(entries), elf())
        self.assertIn("sbin/awk", r.inspect(after)["awk"])

    def test_dynamic_interpreter_dynamic_segment_wrong_arch_and_WX_rejected(self):
        for data in (elf(segment=3), elf(segment=2), elf(machine=62), elf(flags=7), elf(flags=4)):
            with self.subTest(data=data[16:20]), self.assertRaises(ValueError):
                r.embed(image(self.entries), data)

    def test_truncated_program_headers_or_segment_rejected(self):
        bad = bytearray(elf())
        struct.pack_into("<Q", bad, 72, 127)
        for data in (elf()[:70], bytes(bad)):
            with self.assertRaises(ValueError):
                r.static_arm64(data)

    def test_symlink_unowned_or_nonexecutable_awk_rejected(self):
        for mode, uid in ((0o120777, 0), (0o100644, 0), (0o100755, 2000)):
            awk = entry("system/bin/awk", elf(), mode)
            awk[1][2] = uid
            with self.subTest(mode=mode, uid=uid), self.assertRaises(ValueError):
                r.inspect(image(self.entries[:-1] + [awk] + self.entries[-1:]))

    def test_identical_existing_awk_keeps_image_byte_identical(self):
        before, _ = r.embed(image(self.entries), elf())
        after, delta = r.embed(before, elf())
        self.assertEqual(before, after)
        self.assertEqual(delta["added_entries"], [])

    def test_different_existing_awk_not_silently_overwritten(self):
        before, _ = r.embed(image(self.entries), elf())
        with self.assertRaisesRegex(ValueError, "differs"):
            r.embed(before, elf() + b"changed")

    def test_trailer_corruption_or_unexpected_tail_rejected(self):
        before = image(self.entries)
        for data in (before[:-1], before + b"unexpected"):
            with self.assertRaises(ValueError):
                r.unpack(data)
        self.assertEqual(r.unpack(before + b"\0" * 2048)["entries"], self.entries)

if __name__ == "__main__":
    unittest.main()
