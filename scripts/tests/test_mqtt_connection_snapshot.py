"""Exercise MQTT's own connection generation without reading vendor state."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/protocols/mqtt_protocol.cc"


class MqttConnectionSnapshotTest(unittest.TestCase):
    def test_connection_snapshot_uses_callback_generation(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")

        source = SOURCE.read_text()
        begin = source.index("bool MqttProtocol::IsMqttConnected(")
        end = source.index("void MqttProtocol::ArmReconnectTimer(", begin)
        actual_method = source[begin:end]
        harness = r'''
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

struct Mqtt {
    bool IsConnected() { std::abort(); }
};
class MqttProtocol {
public:
    bool IsMqttConnected(const std::shared_ptr<Mqtt>&, uint64_t) const;
    std::atomic<uint64_t> mqtt_generation_{1};
    std::atomic<uint64_t> mqtt_connected_generation_{0};
};
''' + actual_method + r'''
int main() {
    MqttProtocol protocol;
    auto client = std::make_shared<Mqtt>();
    if (protocol.IsMqttConnected(nullptr, 1)) return 1;
    if (protocol.IsMqttConnected(client, 1)) return 2;
    protocol.mqtt_connected_generation_.store(1);
    if (!protocol.IsMqttConnected(client, 1)) return 3;
    protocol.mqtt_generation_.store(2);
    if (protocol.IsMqttConnected(client, 1)) return 4;
    if (protocol.IsMqttConnected(client, 2)) return 5;
    protocol.mqtt_connected_generation_.store(2);
    if (!protocol.IsMqttConnected(client, 2)) return 6;
    protocol.mqtt_connected_generation_.store(0);
    if (protocol.IsMqttConnected(client, 2)) return 7;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "mqtt_connection_snapshot.cc"
            binary = Path(directory) / "mqtt_connection_snapshot"
            cpp.write_text(harness)
            subprocess.run([compiler, "-std=c++23", cpp, "-o", binary], check=True,
                           capture_output=True, text=True)
            subprocess.run([binary], check=True, capture_output=True, text=True,
                           timeout=10)

    def test_no_vendor_connection_state_read_in_protocol(self):
        self.assertNotIn("->IsConnected(", SOURCE.read_text())


if __name__ == "__main__":
    unittest.main()
