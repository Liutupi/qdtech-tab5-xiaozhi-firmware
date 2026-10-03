"""Exercise WebSocket::Abort against an in-flight blocked TCP send."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "vendor_overrides" / "78__esp-ml307"
TESTS = Path(__file__).resolve().parent


class WebSocketAbortTest(unittest.TestCase):
    def test_protocol_cancel_signals_without_teardown(self):
        source = (ROOT / "main" / "protocols" / "websocket_protocol.cc").read_text()
        cancel = source.split("void WebsocketProtocol::CancelOpen() {", 1)[1].split(
            "void WebsocketProtocol::CloseAudioChannel", 1)[0]
        self.assertIn("websocket_->Interrupt();", cancel)
        self.assertNotIn("->Abort()", cancel)
        self.assertNotIn("->Disconnect()", cancel)

    def test_abort_releases_blocked_sender(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "websocket_abort_smoke"
            subprocess.run(
                [compiler, "-std=c++23", "-pthread", "-O1",
                 "-I", str(TESTS / "esp_tcp_stubs"),
                 "-I", str(COMPONENT / "include"),
                 str(TESTS / "websocket_abort_smoke.cc"),
                 str(COMPONENT / "src/web_socket.cc"), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("WebSocket Interrupt and Abort released blocked operations",
                          result.stdout)


if __name__ == "__main__":
    unittest.main()
