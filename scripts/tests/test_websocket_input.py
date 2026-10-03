"""Host checks for bounded WebSocket input and non-throwing URL parsing."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "vendor_overrides" / "78__esp-ml307"
TESTS = Path(__file__).resolve().parent


class WebSocketInputTest(unittest.TestCase):
    def test_url_and_receive_boundaries(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            for psram_malloc in (False, True):
                with self.subTest(psram_malloc=psram_malloc):
                    binary = Path(directory) / f"websocket_input_{int(psram_malloc)}"
                    subprocess.run(
                        [compiler, "-std=c++23", "-fno-exceptions", "-pthread", "-O1",
                         f"-DCONFIG_SPIRAM_USE_MALLOC={int(psram_malloc)}",
                         "-I", str(TESTS / "esp_tcp_stubs"),
                         "-I", str(COMPONENT / "include"),
                         str(TESTS / "websocket_input_smoke.cc"),
                         str(COMPONENT / "src/web_socket.cc"), "-o", str(binary)],
                        check=True, capture_output=True, text=True,
                    )
                    result = subprocess.run([str(binary)], check=True, capture_output=True,
                                            text=True, timeout=10)
                    self.assertIn("WebSocket URL and receive limits passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
