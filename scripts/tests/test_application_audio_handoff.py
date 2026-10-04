"""Exercise the real Application open/close handoff with a controllable worker."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def application_harness(source):
    def section(start, end):
        begin = source.index(start)
        return source[begin:source.index(end, begin)]

    close = section('void Application::RequestProtocolClose(',
                    'void Application::QueueProtocolOpen(')
    open_channel = section('void Application::QueueProtocolOpen(',
                           'void Application::QueuePendingMcpMessage(')
    listening = section('void Application::StartListeningAudio()',
                        'void Application::ConfigureWakeWordForListening()')

    return r'''
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
#define MAIN_EVENT_PLAYBACK_DRAINED 1
static void xEventGroupSetBits(int, int) {}

#include "reply_wait_state.h"
enum ListeningMode { kListeningModeAutoStop, kListeningModeManualStop };
enum class PowerSaveLevel { PERFORMANCE, LOW_POWER };
struct Camera { void PauseStream() {} };
struct AudioCodec { int output_sample_rate() const { return 24000; } };
struct Board {
    static Board& GetInstance() { static Board instance; return instance; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
    void PrepareForNetwork() {}
    Camera* GetCamera() { return nullptr; }
    AudioCodec* GetAudioCodec() { return &codec; }
    AudioCodec codec;
};
struct BackgroundTask {
    std::deque<std::function<void()>> jobs;
    void Schedule(std::function<void()> job) { jobs.push_back(std::move(job)); }
    void RunAll() {
        while (!jobs.empty()) {
            auto job = std::move(jobs.front());
            jobs.pop_front();
            job();
        }
    }
};
struct Protocol {
    bool opened = false;
    bool canceled = false;
    int opens = 0;
    int closes = 0;
    int starts = 0;
    std::function<void()> during_open;
    bool IsAudioChannelOpened() const { return opened; }
    bool OpenAudioChannel() {
        ++opens;
        if (during_open) during_open();
        if (canceled) return false;
        opened = true;
        return true;
    }
    void CancelOpen() { canceled = true; }
    void CloseAudioChannel(bool) { ++closes; opened = false; canceled = false; }
    int server_sample_rate() const { return 24000; }
    void SendStartListening(ListeningMode) { ++starts; }
};
struct AudioService {
    int voice_enables = 0;
    int popups = 0;
    void EnableVoiceProcessing(bool value) { if (value) ++voice_enables; }
    void PlaySound(const char*) { ++popups; }
};
namespace Lang { namespace Sounds { constexpr const char* OGG_POPUP = "popup"; } }
enum class OutboundKind { Start };

class Application {
public:
    explicit Application(std::shared_ptr<Protocol> protocol)
        : protocol_(std::move(protocol)), background_task_(&background) {}

    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState value) { state = value; }
    bool IsExternalAudioActive() const { return external; }
    bool IsCurrentProtocol(const std::shared_ptr<Protocol>& protocol, uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(protocol_mutex_);
        return protocol && protocol_ == protocol && protocol_epoch_.load() == epoch;
    }
    bool IsCurrentProtocol(const std::weak_ptr<Protocol>& protocol, uint64_t epoch) const {
        std::lock_guard<std::mutex> lock(protocol_mutex_);
        if (!protocol_ || protocol_epoch_.load() != epoch) return false;
        const std::weak_ptr<Protocol> current = protocol_;
        return !protocol.owner_before(current) && !current.owner_before(protocol);
    }
    template <typename Fn> bool TryWithProtocol(Fn&& fn) {
        std::unique_lock<std::mutex> io_lock(protocol_io_mutex_, std::try_to_lock);
        if (!io_lock.owns_lock() || protocol_work_pending_.load() != 0) return false;
        std::shared_ptr<Protocol> protocol;
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            protocol = protocol_;
        }
        if (!protocol) return false;
        fn(*protocol);
        return true;
    }
    void FlushPendingMcpMessages(uint64_t) {}
    void FlushPendingProtocolActions() {}
    void ClearOutboundMessages(bool = false, bool = false) {}
    bool QueueOutboundControl(OutboundKind, const std::string& = "",
                              ListeningMode mode = kListeningModeAutoStop) {
        return TryWithProtocol([mode](Protocol& protocol) { protocol.SendStartListening(mode); });
    }
    void Schedule(std::function<void()> callback) { main_jobs.push_back(std::move(callback)); }
    void RunMain() {
        while (!main_jobs.empty()) {
            auto job = std::move(main_jobs.front());
            main_jobs.pop_front();
            job();
        }
    }
    void ConfigureWakeWordForListening() {}
    void RequestProtocolClose(bool reset = false, bool send_goodbye = true);
    void QueueProtocolOpen(std::function<void()> on_opened);
    void FinalizeProtocolOpen();
    void StartListeningAudio();

    DeviceState state = kDeviceStateConnecting;
    bool external = false;
    bool pending_listening_start_ = false;
    bool play_popup_on_listening_ = false;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AudioService audio_service_;
    ReplyWaitState reply_wait_;
    BackgroundTask background;
    std::deque<std::function<void()>> main_jobs;
    mutable std::mutex protocol_mutex_;
    std::mutex protocol_io_mutex_;
    std::mutex protocol_reset_mutex_;
    std::condition_variable protocol_reset_cv_;
    std::shared_ptr<Protocol> protocol_;
    std::atomic<uint64_t> protocol_epoch_{0};
    std::atomic<unsigned> protocol_work_pending_{0};
    std::atomic<bool> suppress_close_callback_{false};
    bool protocol_open_queued_ = false;
    uint64_t protocol_open_epoch_ = 0;
    uint64_t protocol_closed_during_open_epoch_ = 0;
    struct PendingOpenCompletion {
        std::weak_ptr<Protocol> identity;
        uint64_t epoch;
        bool opened;
    };
    std::optional<PendingOpenCompletion> pending_open_completion_;
    std::function<void()> protocol_open_completion_;
    unsigned protocol_resets_pending_ = 0;
    int event_group_ = 0;
    BackgroundTask* background_task_;
    BackgroundTask* outbound_task_ = nullptr;
};
''' + close + open_channel + listening + r'''
int main() {
    // Two requests during one slow handshake must preserve the most recent intent.
    auto p1 = std::make_shared<Protocol>();
    Application duplicate(p1);
    int old_intent = 0, new_intent = 0;
    duplicate.QueueProtocolOpen([&] { ++old_intent; });
    duplicate.QueueProtocolOpen([&] { ++new_intent; });
    duplicate.background.RunAll();
    duplicate.RunMain();
    if (p1->opens != 1 || old_intent != 0 || new_intent != 1 ||
        duplicate.protocol_work_pending_.load() != 0) return 1;

    // Cancellation before worker entry must not connect a stale channel.
    auto p2 = std::make_shared<Protocol>();
    Application before(p2);
    int stale_intent = 0;
    before.QueueProtocolOpen([&] { ++stale_intent; });
    before.reply_wait_.Voice(true);
    before.reply_wait_.Voice(false);
    if (!before.reply_wait_.Recognized(0, kDeviceStateListening)) return 9;
    before.RequestProtocolClose(false, false);
    if (before.reply_wait_.Waiting(1, kDeviceStateListening)) return 10;
    before.background.RunAll();
    before.RunMain();
    if (p2->opens != 0 || p2->closes != 1 || stale_intent != 0) return 2;

    // A cancellation delivered from inside a blocked open must invalidate its result.
    auto p3 = std::make_shared<Protocol>();
    Application during(p3);
    int late_intent = 0;
    p3->during_open = [&] {
        during.RequestProtocolClose(false, false);
        during.SetDeviceState(kDeviceStateIdle);
    };
    during.QueueProtocolOpen([&] { ++late_intent; });
    during.background.RunAll();
    during.RunMain();
    if (p3->opens != 1 || p3->closes != 1 || late_intent != 0) return 3;

    // A network job between worker success and main completion is busy, not closed.
    auto pbusy = std::make_shared<Protocol>();
    Application busy(pbusy);
    int busy_intent = 0;
    busy.QueueProtocolOpen([&] { ++busy_intent; });
    busy.background.RunAll();
    busy.protocol_work_pending_.store(1);
    busy.RunMain();
    if (busy_intent != 0 || !busy.pending_open_completion_) return 4;
    busy.protocol_work_pending_.store(0);
    busy.FinalizeProtocolOpen();
    if (busy_intent != 1 || busy.pending_open_completion_) return 5;

    // A passive disconnect after worker success must not enter Listening.
    auto pclosed = std::make_shared<Protocol>();
    Application closed(pclosed);
    int closed_intent = 0;
    closed.QueueProtocolOpen([&] { ++closed_intent; });
    closed.background.RunAll();
    pclosed->opened = false;
    closed.protocol_closed_during_open_epoch_ = closed.protocol_open_epoch_;
    closed.RunMain();
    if (closed_intent != 0 || closed.state != kDeviceStateIdle) return 6;

    // External playback keeps the microphone disabled until focus is returned.
    auto p4 = std::make_shared<Protocol>();
    Application listening(p4);
    listening.state = kDeviceStateListening;
    listening.external = true;
    listening.reply_wait_.Voice(true);
    listening.reply_wait_.Voice(false);
    if (!listening.reply_wait_.Recognized(0, kDeviceStateListening)) return 11;
    listening.StartListeningAudio();
    if (listening.reply_wait_.Waiting(1, kDeviceStateListening)) return 12;
    if (!listening.pending_listening_start_ || p4->starts ||
        listening.audio_service_.voice_enables) return 7;
    listening.external = false;
    listening.StartListeningAudio();
    if (p4->starts != 1 || listening.audio_service_.voice_enables != 1) return 8;
}
'''


class ApplicationHandoffTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_open_cancel_and_external_audio_interleavings(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/application.cc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'application.cc'
            binary = Path(directory) / 'application'
            test.write_text(application_harness(source))
            subprocess.run(['c++', '-std=c++17', '-pthread', '-I', str(root / 'main'), str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
