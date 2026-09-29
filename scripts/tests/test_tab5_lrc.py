"""Compile and exercise the Tab5 LRC parser on the host."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class Tab5LrcTests(unittest.TestCase):
    def test_timed_plain_and_invalid_lyrics(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ host compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "tab5-lrc-smoke"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                 str(BOARD / "tab5_lrc.cc"),
                 str(Path(__file__).with_name("tab5_lrc_smoke.cc")),
                 "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
