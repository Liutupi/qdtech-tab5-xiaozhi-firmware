"""Exercise the source-owned ESP TCP receive-task lifetime with real sockets."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "vendor_overrides" / "78__esp-ml307"
TESTS = Path(__file__).resolve().parent


class EspTcpOverrideTest(unittest.TestCase):
    def test_passive_disconnect_destructor_joins_receive_task(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "esp_tcp_lifecycle_smoke"
            subprocess.run(
                [compiler, "-std=c++23", "-pthread", "-O1", "-I", str(TESTS / "esp_tcp_stubs"),
                 "-I", str(COMPONENT / "include"), "-I", str(COMPONENT / "src/esp"),
                 str(TESTS / "esp_tcp_lifecycle_smoke.cc"),
                 str(COMPONENT / "src/esp/esp_tcp.cc"),
                 str(COMPONENT / "src/esp/esp_ssl.cc"),
                 str(COMPONENT / "src/http_client.cc"),
                 str(COMPONENT / "src/web_socket.cc"), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                                    timeout=25)
            self.assertIn("24 TCP joins, 2 WebSocket, 1 HTTP and 2 TLS lifecycle cases passed", result.stdout)
            self.assertIn("TCP/TLS Interrupt and WANT_WRITE deadline passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
