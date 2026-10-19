"""NABO FM radio page: action policy, pack writer/reader contract, asset names."""
import ctypes.util
import importlib.util
import re
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

try:
    import numpy as np
except ImportError:  # the packaging tool needs numpy; the policy test does not
    np = None

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
TESTS = Path(__file__).parent
SCRIPT = ROOT / "scripts/prepare_nabo_radio_actions.py"


def load_script():
    if np is None:
        raise unittest.SkipTest("numpy unavailable")
    spec = importlib.util.spec_from_file_location("prepare_nabo_radio_actions", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compile_smoke(source, binary):
    compiler = shutil.which("c++")
    if not compiler:
        raise unittest.SkipTest("C++ host compiler unavailable")
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                    "-I", str(TESTS), str(source), "-o", str(binary)], check=True)


class NaboRadioTests(unittest.TestCase):
    def test_mood_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "mood"
            compile_smoke(TESTS / "nabo_radio_mood_smoke.cc", binary)
            subprocess.run([str(binary)], check=True)

    def test_action_names_match_firmware(self):
        header = (BOARD / "nabo_radio_mood.h").read_text()
        names = re.search(r"kNames\[kActionCount\] = \{(.*?)\};", header, re.S).group(1)
        firmware = re.findall(r'"([a-z_]+)"', names)
        script = [action for action, _, _ in load_script().ACTIONS]
        self.assertEqual(firmware, script)

    def test_pack_round_trip(self):
        if not ctypes.util.find_library("lz4"):
            self.skipTest("liblz4 unavailable")
        tool = load_script()
        rng = np.random.default_rng(7)
        arts = []
        for n in range(3):
            art = np.zeros((tool.HEIGHT, tool.WIDTH, 4), np.uint8)
            art[100 + n:300, 120:360 - n * 10] = rng.integers(0, 255, (200 - n, 240 - n * 10, 4))
            art[100 + n:300, 120:360 - n * 10, 3] = 255
            arts.append(art)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            report = tool.write_pack(tmp / "tune.nab", arts, tool.LOOP_MS, tool.Codec(None))
            self.assertEqual(report["frames"], 3)
            self.assertEqual(report["duration_ms"], 3 * tool.LOOP_MS)
            still = tool.write_still(tmp / "mug.nab", arts[0][90:310, 110:370], tool.Codec(None))
            self.assertEqual(still["frames"], 1)
            binary = tmp / "pack"
            compile_smoke(TESTS / "nabo_radio_pack_smoke.cc", binary)
            out = subprocess.run([str(binary), str(tmp / "tune.nab")], check=True,
                                 capture_output=True, text=True).stdout.split("\n")
            for n, line in enumerate(filter(None, out)):
                at, x, y, w, h, crc = line.split()
                x, y, w, h = int(x), int(y), int(w), int(h)
                self.assertEqual(int(at), n * tool.LOOP_MS)
                # The crop keeps every visible pixel and the decoded bytes match the encoder.
                self.assertEqual((x, y), (118, 98 + n))
                expected = tool.encode(arts[n][y:y + h, x:x + w])
                self.assertEqual(int(crc, 16), zlib.crc32(expected))
            subprocess.run([str(binary), str(tmp / "mug.nab")], check=True, capture_output=True)

    def test_station_font_covers_builtin_names(self):
        tool = load_script()
        symbols = tool.font_symbols([])
        names = re.findall(r'\{"([^"]+)", \{"http', (BOARD / "radio_service.cc").read_text())
        self.assertGreater(len(names), 20)
        missing = {c for name in names for c in name if c not in symbols}
        self.assertEqual(missing, set())


if __name__ == "__main__":
    unittest.main()
