"""Run the real ICU page callbacks with a minimal host LVGL model."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class IcuPageTests(unittest.TestCase):
    def test_manual_and_voice_results_are_not_mixed(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ host compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "icu-page-smoke"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(Path(__file__).with_name("icu_page_fakes")),
                 "-I", str(BOARD),
                 str(BOARD / "icu_calculators.cc"),
                 str(BOARD / "tab5_icu_page.cc"),
                 str(Path(__file__).with_name("icu_page_smoke.cc")),
                 "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
