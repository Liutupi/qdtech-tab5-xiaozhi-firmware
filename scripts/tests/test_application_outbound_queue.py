"""Run the real Application outbound queue methods against a blocking transport."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def outbound_harness(source):
    begin = source.index('bool Application::QueueOutboundAudio(')
    end = source.index('void Application::CloseAudioChannelAsync(', begin)
    methods = source[begin:end]
    return r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define ESP_LOGW(...) ((void)0)
#define MAIN_EVENT_PLAYBACK_DRAINED 1
#define MAX_SEND_PACKETS_IN_QUEUE 48
static void xEventGroupSetBits(int, int) {}

enum ListeningMode { kListeningModeAutoStop, kListeningModeManualStop };
enum AbortReason { kAbortReasonNone, kAbortReasonWakeWordDetected };
enum DeviceState { kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening,
                   kDeviceStateSpeaking };
struct AudioStreamPacket { std::vector<uint8_t> payload; };
struct Protocol {
    std::mutex mutex;
    std::condition_variable cv;
    bool block_audio = false;
    bool entered_audio = false;
    bool release_audio = false;
    bool fail_audio = false;
    bool fail_control = false;
    std::vector<std::string> sent;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
        std::unique_lock<std::mutex> lock(mutex);
        sent.push_back("audio:" + std::to_string(packet->payload.at(0)));
        if (block_audio) {
            entered_audio = true;
            cv.notify_all();
            cv.wait(lock, [this]() { return release_audio; });
            block_audio = false;
        }
        return !fail_audio;
    }
    bool SendWakeWordDetected(const std::string& text) {
        sent.push_back("wake:" + text); return !fail_control;
    }
    bool SendStartListening(ListeningMode) {
        sent.push_back("start"); return !fail_control;
    }
    bool SendStopListening() {
        sent.push_back("stop"); return !fail_control;
    }
    bool SendAbortSpeaking(AbortReason) {
        sent.push_back("abort"); return !fail_control;
    }
    bool SendMcpMessage(const std::string& text) {
        sent.push_back("mcp:" + text); return !fail_control;
    }
    void WaitForAudio() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return entered_audio; });
    }
    void ReleaseAudio() {
        std::lock_guard<std::mutex> lock(mutex);
        release_audio = true;
        cv.notify_all();
    }
};
struct BackgroundTask {
    std::thread worker;
    void Schedule(std::function<void()> job) {
        assert(!worker.joinable());
        worker = std::thread(std::move(job));
    }
    void Wait() { if (worker.joinable()) worker.join(); }
    ~BackgroundTask() { Wait(); }
};

class Application {
public:
    enum class OutboundKind { Audio, WakeWord, Start, Stop, Abort, Mcp };
    struct OutboundMessage {
        OutboundKind kind;
        std::weak_ptr<Protocol> identity;
        uint64_t epoch;
        std::unique_ptr<AudioStreamPacket> audio;
        bool wake_word_data = false;
        std::string text;
        ListeningMode listening_mode = kListeningModeAutoStop;
        AbortReason abort_reason = kAbortReasonNone;
    };
    explicit Application(std::shared_ptr<Protocol> protocol)
        : protocol_(std::move(protocol)), outbound_task_(std::make_unique<BackgroundTask>()) {}
    template <typename Fn> bool TryWithProtocol(Fn&& fn) {
        std::lock_guard<std::mutex> lock(protocol_io_mutex_);
        if (!protocol_) return false;
        fn(*protocol_);
        return true;
    }
    bool IsCurrentProtocol(const std::weak_ptr<Protocol>& identity, uint64_t epoch) {
        std::lock_guard<std::mutex> lock(protocol_mutex_);
        return protocol_epoch_.load() == epoch && !identity.expired() &&
               identity.lock() == protocol_;
    }
    void Schedule(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(main_mutex_);
        main_jobs_.push_back(std::move(callback));
    }
    void RunMain() {
        while (true) {
            std::function<void()> callback;
            {
                std::lock_guard<std::mutex> lock(main_mutex_);
                if (main_jobs_.empty()) break;
                callback = std::move(main_jobs_.front());
                main_jobs_.pop_front();
            }
            callback();
        }
    }
    void RequestProtocolClose(bool, bool) {
        protocol_epoch_.fetch_add(1);
        ClearOutboundMessages();
        ++closes;
    }
    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState value) { state = value; }
    bool QueueOutboundAudio(std::unique_ptr<AudioStreamPacket> packet, bool wake_word_data = false);
    bool QueueOutboundControl(OutboundKind kind, const std::string& text = "",
                              ListeningMode mode = kListeningModeAutoStop,
                              AbortReason reason = kAbortReasonNone);
    void ProcessOutboundMessages();
    void ClearOutboundMessages(bool audio_only = false, bool preserve_wake_word_data = false);

    std::mutex protocol_mutex_;
    std::mutex protocol_io_mutex_;
    std::shared_ptr<Protocol> protocol_;
    std::atomic<uint64_t> protocol_epoch_{1};
    std::atomic<unsigned> protocol_work_pending_{0};
    std::unique_ptr<BackgroundTask> outbound_task_;
    std::mutex outbound_mutex_;
    std::deque<OutboundMessage> outbound_messages_;
    size_t outbound_audio_count_ = 0;
    size_t outbound_audio_bytes_ = 0;
    size_t outbound_control_count_ = 0;
    size_t outbound_mcp_count_ = 0;
    size_t outbound_control_bytes_ = 0;
    bool outbound_pump_queued_ = false;
    std::atomic<uint64_t> outbound_failed_epoch_{0};
    std::mutex main_mutex_;
    std::deque<std::function<void()>> main_jobs_;
    int event_group_ = 0;
    int closes = 0;
    int ui_ticks = 0;
    DeviceState state = kDeviceStateListening;
};
static std::unique_ptr<AudioStreamPacket> Packet(uint8_t value) {
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->payload.push_back(value);
    return packet;
}
''' + methods + r'''
int main() {
    // A blocked network send must not hold up the main event loop. FIFO keeps
    // listen/start controls between the audio packets around them.
    auto protocol = std::make_shared<Protocol>();
    protocol->block_audio = true;
    Application app(protocol);
    if (!app.QueueOutboundAudio(Packet(1))) return 1;
    protocol->WaitForAudio();
    if (!app.QueueOutboundControl(Application::OutboundKind::Start) ||
        !app.QueueOutboundAudio(Packet(2))) return 2;
    ++app.ui_ticks;
    if (app.ui_ticks != 1 || app.outbound_messages_.size() != 2) return 3;
    protocol->ReleaseAudio();
    app.outbound_task_->Wait();
    if (protocol->sent != std::vector<std::string>{"audio:1", "start", "audio:2"}) return 4;

    // A stop event discards queued live audio, leaving only the in-flight
    // packet before the stop control reaches the transport.
    auto stopped = std::make_shared<Protocol>();
    stopped->block_audio = true;
    Application stop(stopped);
    if (!stop.QueueOutboundAudio(Packet(1))) return 5;
    stopped->WaitForAudio();
    if (!stop.QueueOutboundControl(Application::OutboundKind::Mcp, "prior")) return 20;
    for (int i = 2; i <= 70; ++i) {
        if (!stop.QueueOutboundAudio(Packet(static_cast<uint8_t>(i)))) return 6;
    }
    if (stop.outbound_audio_count_ != 16 || stop.outbound_messages_.size() != 17) return 7;
    stop.ClearOutboundMessages(true);
    if (!stop.QueueOutboundControl(Application::OutboundKind::Stop)) return 8;
    stopped->ReleaseAudio();
    stop.outbound_task_->Wait();
    if (stopped->sent != std::vector<std::string>{"audio:1", "stop", "mcp:prior"}) return 9;

    // Reset/close invalidates queued controls and releases their memory before
    // the current socket operation finally returns.
    auto reset_proto = std::make_shared<Protocol>();
    reset_proto->block_audio = true;
    Application reset(reset_proto);
    if (!reset.QueueOutboundAudio(Packet(1))) return 10;
    reset_proto->WaitForAudio();
    if (!reset.QueueOutboundControl(Application::OutboundKind::Mcp, "old")) return 11;
    reset.RequestProtocolClose(false, false);
    if (!reset.outbound_messages_.empty()) return 12;
    reset_proto->ReleaseAudio();
    reset.outbound_task_->Wait();
    if (reset_proto->sent != std::vector<std::string>{"audio:1"}) return 13;

    // Control saturation is bounded. Audio send failure rejects new packets,
    // drops the unsent tail, and asks the main task to close the channel.
    auto saturated = std::make_shared<Protocol>();
    saturated->block_audio = true;
    Application full(saturated);
    if (!full.QueueOutboundAudio(Packet(1))) return 14;
    saturated->WaitForAudio();
    for (int i = 0; i < 4; ++i) {
        if (!full.QueueOutboundControl(Application::OutboundKind::Mcp, "x")) return 15;
    }
    if (full.QueueOutboundControl(Application::OutboundKind::Mcp, "overflow")) return 16;
    if (!full.QueueOutboundControl(Application::OutboundKind::Stop) ||
        full.outbound_mcp_count_ != 4 || full.outbound_control_count_ != 5) return 19;
    {
        std::lock_guard<std::mutex> lock(saturated->mutex);
        saturated->fail_audio = true;
    }
    saturated->ReleaseAudio();
    full.outbound_task_->Wait();
    if (full.outbound_failed_epoch_.load() != 1 || !full.outbound_messages_.empty() ||
        full.QueueOutboundAudio(Packet(2))) return 17;
    full.RunMain();
    if (full.closes != 1 || full.state != kDeviceStateIdle) return 18;

    // Wake-word preroll survives the start-listening cleanup; ordinary stale
    // microphone packets do not. Both retain their original FIFO position.
    auto preroll_proto = std::make_shared<Protocol>();
    preroll_proto->block_audio = true;
    Application preroll(preroll_proto);
    if (!preroll.QueueOutboundAudio(Packet(1))) return 21;
    preroll_proto->WaitForAudio();
    if (!preroll.QueueOutboundAudio(Packet(2), true) ||
        !preroll.QueueOutboundAudio(Packet(3))) return 22;
    preroll.ClearOutboundMessages(true, true);
    if (preroll.outbound_audio_count_ != 1) return 23;
    preroll_proto->ReleaseAudio();
    preroll.outbound_task_->Wait();
    if (preroll_proto->sent != std::vector<std::string>{"audio:1", "audio:2"}) return 24;

    // Resource-constrained non-Tab5 boards retain the previous synchronous
    // path and do not allocate a send worker.
    auto legacy_proto = std::make_shared<Protocol>();
    Application legacy(legacy_proto);
    legacy.outbound_task_.reset();
    if (!legacy.QueueOutboundAudio(Packet(7)) ||
        !legacy.QueueOutboundControl(Application::OutboundKind::Start)) return 25;
    if (legacy_proto->sent != std::vector<std::string>{"audio:7", "start"}) return 26;

    // The transport can reject a listen control after it was queued. That
    // failure closes only its own session instead of recording fake success.
    auto control_proto = std::make_shared<Protocol>();
    control_proto->fail_control = true;
    Application control(control_proto);
    if (!control.QueueOutboundControl(Application::OutboundKind::Start)) return 27;
    control.outbound_task_->Wait();
    control.RunMain();
    if (control.closes != 1 || control.state != kDeviceStateIdle) return 28;

    // A failed old send must never delete a replacement session's Start.
    auto old_proto = std::make_shared<Protocol>();
    old_proto->block_audio = true;
    old_proto->fail_audio = true;
    Application replaced(old_proto);
    if (!replaced.QueueOutboundAudio(Packet(1))) return 29;
    old_proto->WaitForAudio();
    replaced.RequestProtocolClose(false, false);
    auto new_proto = std::make_shared<Protocol>();
    {
        std::lock_guard<std::mutex> lock(replaced.protocol_mutex_);
        replaced.protocol_ = new_proto;
    }
    if (!replaced.QueueOutboundControl(Application::OutboundKind::Start)) return 30;
    old_proto->ReleaseAudio();
    replaced.outbound_task_->Wait();
    replaced.RunMain();
    if (new_proto->sent != std::vector<std::string>{"start"} ||
        replaced.closes != 1 || replaced.outbound_failed_epoch_.load() == 2) return 31;

    Application legacy_failed(control_proto);
    legacy_failed.outbound_task_.reset();
    if (legacy_failed.QueueOutboundControl(Application::OutboundKind::Start)) return 32;
}
'''


class ApplicationOutboundQueueTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_blocked_send_order_limits_and_cancellation(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/application.cc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'application_outbound.cc'
            binary = Path(directory) / 'application_outbound'
            test.write_text(outbound_harness(source))
            subprocess.run(['c++', '-std=c++17', '-pthread', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
