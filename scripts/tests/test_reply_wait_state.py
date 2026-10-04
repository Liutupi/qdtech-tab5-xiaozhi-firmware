import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
class ReplyWaitStateTests(unittest.TestCase):
    def test_recognition_capture_interruptions_timeout_and_work_clock(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/"reply-wait"
            subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",
                "-I",str(ROOT/"main"),"-I",str(ROOT/"main/boards/qdtech/tab5"),
                str(Path(__file__).with_name("reply_wait_smoke.cc")),"-o",str(output)],check=True)
            subprocess.run([str(output)],check=True)
if __name__ == "__main__":
    unittest.main()
