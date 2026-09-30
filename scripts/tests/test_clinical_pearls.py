import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ClinicalPearlsTest(unittest.TestCase):
    def test_generated_file_is_fresh_and_fits_font(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts/generate_clinical_pearls.py"), "--check"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
