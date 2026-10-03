"""Exercise RadioService's real generation-gated terminal music transition."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/radio_service.cc"


def make_harness(source: str) -> str:
    begin = source.index("bool RadioService::FinishCustomUrlIfCurrent(")
    end = source.index("void RadioService::SetUi(", begin)
    transition = source[begin:end]
    return r'''
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RadioService {
public:
    std::mutex submission_mutex_;
    std::atomic<uint32_t> stream_generation_{1};
    std::atomic<bool> replacement_pending_{false};
    std::atomic<bool> play_requested_{true};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> playing_custom_url_{true};
    std::atomic<bool> playback_release_pending_{false};
    std::atomic<int> music_playback_state_{1};
    bool custom_url_stream_completed_ = false;
    int reconnect_attempt_ = 3;
    std::mutex events_mutex_;
    std::condition_variable event_ready_;
    std::vector<std::string> events_;
    bool hold_event_ = false;
    bool event_entered_ = false;
    bool release_event_ = false;

    bool FinishCustomUrlIfCurrent(uint32_t generation, bool completed,
                                  const char* detail);
    void SetUi(const char* state, const char* detail) {
        std::unique_lock<std::mutex> lock(events_mutex_);
        events_.emplace_back(std::string(state) + ":" + detail);
        if (hold_event_) {
            event_entered_ = true;
            event_ready_.notify_all();
            event_ready_.wait(lock, [this] { return release_event_; });
        }
    }
    uint32_t SubmitNewSong() {
        std::lock_guard<std::mutex> lock(submission_mutex_);
        const auto generation = stream_generation_.fetch_add(1) + 1;
        replacement_pending_ = false;
        play_requested_ = true;
        stop_requested_ = false;
        playing_custom_url_ = true;
        playback_release_pending_ = false;
        music_playback_state_ = 1;
        custom_url_stream_completed_ = false;
        reconnect_attempt_ = 0;
        return generation;
    }
};

''' + transition + r'''

int main() {
    RadioService radio;
    const auto first = radio.stream_generation_.load();
    const auto second = radio.SubmitNewSong();
    // A drained old stream cannot overwrite the new song's state or publish
    // its ended event after the replacement request has advanced generation.
    if (radio.FinishCustomUrlIfCurrent(first, true, "Music ended")) return 1;
    if (radio.music_playback_state_ != 1 || !radio.play_requested_ ||
        radio.stop_requested_ || radio.custom_url_stream_completed_ ||
        !radio.events_.empty()) return 2;

    radio.replacement_pending_ = true;
    if (radio.FinishCustomUrlIfCurrent(second, true, "Music ended")) return 3;
    radio.replacement_pending_ = false;
    radio.stop_requested_ = true;
    if (radio.FinishCustomUrlIfCurrent(second, true, "Music ended")) return 4;
    radio.stop_requested_ = false;

    if (!radio.FinishCustomUrlIfCurrent(second, true, "Music ended")) return 5;
    if (radio.music_playback_state_ != 2 || radio.play_requested_ ||
        !radio.stop_requested_ || !radio.playback_release_pending_ ||
        !radio.custom_url_stream_completed_ || radio.reconnect_attempt_ != 0 ||
        radio.events_.size() != 1 || radio.events_[0] != "Stopped:Music ended") return 6;
    if (radio.FinishCustomUrlIfCurrent(second, true, "Music ended")) return 7;

    const auto third = radio.SubmitNewSong();
    if (!radio.FinishCustomUrlIfCurrent(third, false, "Music interrupted")) return 8;
    if (radio.music_playback_state_ != 3 || radio.play_requested_ ||
        !radio.stop_requested_ || radio.custom_url_stream_completed_ ||
        radio.events_.size() != 2 ||
        radio.events_[1] != "Stopped:Music interrupted") return 9;

    const auto fourth = radio.SubmitNewSong();
    radio.hold_event_ = true;
    std::thread player([&] {
        if (!radio.FinishCustomUrlIfCurrent(fourth, true, "Music ended"))
            std::terminate();
    });
    {
        std::unique_lock<std::mutex> lock(radio.events_mutex_);
        radio.event_ready_.wait(lock, [&] { return radio.event_entered_; });
    }
    // The terminal callback must still hold the same lock as new submissions;
    // otherwise a new song can be submitted before an old ended event arrives.
    if (radio.submission_mutex_.try_lock()) {
        radio.submission_mutex_.unlock();
        return 10;
    }
    std::atomic<bool> submitted{false};
    std::thread submitter([&] {
        radio.SubmitNewSong();
        submitted = true;
    });
    {
        std::lock_guard<std::mutex> lock(radio.events_mutex_);
        if (submitted) return 11;
        radio.release_event_ = true;
        radio.event_ready_.notify_all();
    }
    player.join();
    submitter.join();
    if (!submitted || radio.stream_generation_ != fourth + 1 ||
        radio.music_playback_state_ != 1 || !radio.play_requested_ ||
        radio.stop_requested_ || radio.custom_url_stream_completed_ ||
        radio.events_.size() != 3 || radio.events_[2] != "Stopped:Music ended") return 12;
}
'''


class RadioMusicEndGenerationTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_terminal_event_and_state_are_serialized_with_new_song(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "radio_music_end_generation.cc"
            binary = Path(directory) / "radio_music_end_generation"
            cpp.write_text(make_harness(SOURCE.read_text()))
            build = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 str(cpp), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_both_natural_end_sites_use_same_generation_gate(self):
        source = SOURCE.read_text()
        task = source[source.index("void RadioService::Task() {"):
                      source.index("void RadioService::HandleCommand(")]
        play_url = source[source.index("bool RadioService::PlayUrl("):
                          source.index("bool RadioService::IsXiaozhiAudioState()")]
        self.assertIn('FinishCustomUrlIfCurrent(generation, true, "Music ended")', task)
        self.assertIn('FinishCustomUrlIfCurrent(stream_generation, true, "Music ended")',
                      play_url)
        self.assertIn("FinishCustomUrlIfCurrent(\n                    generation, false,", task)
        self.assertNotIn('SetUi("Stopped", "Music ended")', task + play_url)


if __name__ == "__main__":
    unittest.main()
