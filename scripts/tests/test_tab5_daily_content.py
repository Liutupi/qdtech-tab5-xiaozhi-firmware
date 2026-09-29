import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class Tab5DailyContentTest(unittest.TestCase):
    def test_daily_content(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "daily_content_smoke"
            subprocess.run(
                ["c++", "-std=c++17", "-I", str(BOARD),
                 str(ROOT / "scripts/tests/tab5_daily_content_smoke.cc"),
                 str(BOARD / "tab5_daily_content.cc"), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
