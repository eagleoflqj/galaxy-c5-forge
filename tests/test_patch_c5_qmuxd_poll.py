import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('qmux_poll', ROOT / 'scripts/patch-c5-qmuxd-poll.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class QmuxPollTests(unittest.TestCase):
    def fixture(self):
        raw = bytearray(0xcc00)
        for offset, word in m.GUARDS.items():
            struct.pack_into('<I', raw, offset, word)
        return bytes(raw)

    def test_one_immediate_and_preserved_watchdog(self):
        before = self.fixture()
        after = m.replace_verified_word(before)
        self.assertEqual(len(before), len(after))
        self.assertEqual([i for i, (a, b) in enumerate(zip(before, after)) if a != b], [m.SITE])
        self.assertEqual((struct.unpack_from('<I', after, m.SITE)[0] >> 5) & 0xffff, 9)
        for offset, word in m.GUARDS.items():
            if offset != m.SITE:
                self.assertEqual(struct.unpack_from('<I', after, offset)[0], word)

    def test_wrong_poll_read_watchdog_or_already_patched_input_rejected(self):
        for offset in m.GUARDS:
            raw = bytearray(self.fixture())
            raw[offset] ^= 1
            with self.assertRaises(ValueError):
                m.replace_verified_word(raw)
        with self.assertRaises(ValueError):
            m.replace_verified_word(m.replace_verified_word(self.fixture()))

    def test_unverified_proprietary_input_rejected(self):
        with self.assertRaises(ValueError):
            m.transform(self.fixture())
        with self.assertRaises(ValueError):
            m.replace_verified_word(b'\x7fELF')

    def test_actual_native_poll_status_data_and_reset_behavior(self):
        with tempfile.TemporaryDirectory(prefix='c5-smd-poll-') as directory:
            target = Path(directory) / 'poll-test'
            result = subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                                     str(ROOT / 'tests/fixtures/c5-smd-native-poll.c'), '-o', str(target)],
                                    capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            result = subprocess.run([str(target)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__':
    unittest.main()
