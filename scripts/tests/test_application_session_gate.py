"""Exercise the real Application session gate across an open/closed channel."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ApplicationSessionGateTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_actions_wait_for_matching_open_channel(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/application.cc').read_text()
        begin = source.index('void Application::DispatchProtocolAction(')
        end = source.index('bool Application::QueueOutboundAudio(', begin)
        methods = source[begin:end]
        harness = r'''
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#define ESP_LOGW(...) ((void)0)
struct Protocol {
    bool open = false;
    std::string session_id() const { return "current"; }
    bool IsAudioChannelOpened() const { return open; }
};
class Application {
public:
    struct PendingAction {
        std::weak_ptr<Protocol> identity;
        unsigned epoch;
        std::string session_id;
        std::function<void()> action;
    };
    std::shared_ptr<Protocol> protocol_ = std::make_shared<Protocol>();
    std::mutex protocol_mutex_;
    bool protocol_open_queued_ = false;
    unsigned protocol_open_epoch_ = 1;
    std::deque<PendingAction> pending_protocol_actions_;
    bool IsCurrentProtocol(const std::weak_ptr<Protocol>& identity, unsigned epoch) {
        return epoch == 1 && identity.lock() == protocol_;
    }
    template <class F> bool TryWithProtocol(F&& fn) {
        fn(*protocol_);
        return true;
    }
    void DispatchProtocolAction(std::weak_ptr<Protocol>, unsigned,
                                std::string, std::function<void()>);
    void FlushPendingProtocolActions();
};
''' + methods.replace('uint64_t', 'unsigned') + r'''
int main() {
    Application app;
    int ran = 0;
    auto run = [&]() { ++ran; };
    app.DispatchProtocolAction(app.protocol_, 1, "wrong", run);
    app.DispatchProtocolAction(app.protocol_, 1, "current", run);
    if (ran != 0 || !app.pending_protocol_actions_.empty()) return 1;
    app.protocol_open_queued_ = true;
    app.DispatchProtocolAction(app.protocol_, 1, "current", run);
    if (ran != 0 || app.pending_protocol_actions_.size() != 1) return 2;
    app.protocol_->open = true;
    app.protocol_open_queued_ = false;
    app.FlushPendingProtocolActions();
    if (ran != 1 || !app.pending_protocol_actions_.empty()) return 3;
    app.DispatchProtocolAction(app.protocol_, 1, "current", run);
    if (ran != 2) return 4;
    app.protocol_->open = false;
    app.DispatchProtocolAction(app.protocol_, 1, "current", run);
    if (ran != 2) return 5;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'application_session.cc'
            binary = Path(directory) / 'application_session'
            test.write_text(harness)
            subprocess.run(['c++', '-std=c++17', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
