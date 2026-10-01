import pathlib
import subprocess
import tempfile
import unittest


class NaboAnimationTest(unittest.TestCase):
    def test_timeline_and_cancellation(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "nabo-animation"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "main/boards/qdtech/tab5"),
                            str(root / "scripts/tests/nabo_animation_smoke.cc"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
