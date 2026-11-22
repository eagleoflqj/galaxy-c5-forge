import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/patch-c5-thermal-engine-units.py'
spec = importlib.util.spec_from_file_location('thermal_engine_units', SCRIPT)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class ThermalEngineUnitsTests(unittest.TestCase):
    def test_unknown_input_refuses(self):
        with self.assertRaises(ValueError):
            patch.transform(bytes(0xc000))

    def test_cli_refusal_creates_no_output(self):
        with tempfile.TemporaryDirectory() as td:
            source, output = Path(td) / 'input', Path(td) / 'output'
            source.write_bytes(b'unknown')
            done = subprocess.run([sys.executable, str(SCRIPT), '--input', str(source), '--output', str(output)], capture_output=True)
            self.assertNotEqual(done.returncode, 0)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), b'unknown')

    def test_exact_control_flow_and_register_encoding(self):
        # Independently decode B imm26 and MOV (ORR Wd,WZR,Wm).
        address, before, after = patch.EDITS[0]
        self.assertEqual(address, 0xb184)
        instruction = int.from_bytes(after, 'little')
        self.assertEqual(instruction & 0xffe0ffe0, 0x2a0003e0)
        self.assertEqual(instruction & 31, 20)
        self.assertEqual((instruction >> 16) & 31, 0)
        for (address, before, after), target in zip(patch.EDITS[1:], (0xb2f4, 0xb314)):
            instruction = int.from_bytes(after, 'little')
            self.assertEqual(instruction >> 26, 0b000101)
            immediate = instruction & 0x3ffffff
            if immediate & (1 << 25):
                immediate -= 1 << 26
            self.assertEqual(address + immediate * 4, target)
