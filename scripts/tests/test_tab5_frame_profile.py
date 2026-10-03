"""Verify Tab5 frame attribution expiry, overlap, and peak-pixel accounting."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class Tab5FrameProfileTest(unittest.TestCase):
    def test_frame_attribution(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "tab5-frame-profile"
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main/boards/qdtech/tab5"),
                 str(ROOT / "scripts/tests/tab5_frame_profile_smoke.cc"),
                 "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)
