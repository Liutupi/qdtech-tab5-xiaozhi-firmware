"""Compile and run medical calculator unit and boundary checks on the host."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class IcuCalculatorTests(unittest.TestCase):
    def test_calculations_and_invalid_input(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ host compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "icu-calculators-smoke"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                 str(BOARD / "icu_calculators.cc"),
                 str(Path(__file__).with_name("icu_calculators_smoke.cc")),
                 "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
