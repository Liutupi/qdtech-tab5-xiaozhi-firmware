"""Exercise the actual vendor MQTT adapter with fragmented broker events."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "vendor_overrides" / "78__esp-ml307"
TESTS = Path(__file__).resolve().parent


class EspMqttFragmentsTest(unittest.TestCase):
    def _run_smoke(self, defines):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "esp_mqtt_fragment_smoke"
            subprocess.run(
                [compiler, "-std=c++23", "-pthread", "-O1", *defines,
                 "-I", str(TESTS / "esp_mqtt_stubs"),
                 "-I", str(TESTS / "esp_tcp_stubs"),
                 "-I", str(COMPONENT / "include"),
                 "-I", str(COMPONENT / "src" / "esp"),
                 str(TESTS / "esp_mqtt_fragment_smoke.cc"),
                 str(COMPONENT / "src" / "esp" / "esp_mqtt.cc"),
                 "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            result = subprocess.run([binary], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("MQTT fragments and reconnect lifecycle passed", result.stdout)

    def test_without_psram(self):
        self._run_smoke([])

    def test_with_psram_malloc(self):
        self._run_smoke(["-DCONFIG_SPIRAM_USE_MALLOC=1"])


if __name__ == "__main__":
    unittest.main()
