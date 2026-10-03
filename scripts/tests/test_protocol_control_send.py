"""Exercise the real shared Protocol control-message methods on send failure."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ProtocolControlSendTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_control_send_result_reaches_caller(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/protocols/protocol.cc').read_text()
        begin = source.index('bool Protocol::SendAbortSpeaking(')
        end = source.index('bool Protocol::IsTimeout()', begin)
        methods = source[begin:end]
        harness = r'''
#include <string>
#include <vector>

enum AbortReason { kAbortReasonNone, kAbortReasonWakeWordDetected };
enum ListeningMode { kListeningModeAutoStop, kListeningModeManualStop,
                     kListeningModeRealtime };
class Protocol {
public:
    bool send_ok = false;
    std::string session_id_ = "session";
    std::vector<std::string> messages;
    bool SendText(const std::string& text) {
        messages.push_back(text);
        return send_ok;
    }
    bool SendAbortSpeaking(AbortReason reason);
    bool SendWakeWordDetected(const std::string& wake_word);
    bool SendStartListening(ListeningMode mode);
    bool SendStopListening();
    bool SendMcpMessage(const std::string& payload);
};
''' + methods + r'''
int main() {
    Protocol protocol;
    if (protocol.SendAbortSpeaking(kAbortReasonWakeWordDetected)) return 1;
    if (protocol.SendWakeWordDetected("hello")) return 2;
    if (protocol.SendStartListening(kListeningModeAutoStop)) return 3;
    if (protocol.SendStopListening()) return 4;
    if (protocol.SendMcpMessage("{}")) return 5;
    if (protocol.messages.size() != 5) return 6;
    protocol.send_ok = true;
    if (!protocol.SendStartListening(kListeningModeRealtime)) return 7;
    if (protocol.messages.back().find("\"mode\":\"realtime\"") == std::string::npos) return 8;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'protocol_control.cc'
            binary = Path(directory) / 'protocol_control'
            test.write_text(harness)
            subprocess.run(['c++', '-std=c++17', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
