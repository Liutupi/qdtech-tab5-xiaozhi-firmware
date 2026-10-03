"""Exercise the real vision greeting handoff when audio wins the scheduling race."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5/tab5_vision_service.cc"


class Tab5GreetingRetryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_busy_and_stale_greetings_retry_only_for_present_person(self):
        source = SOURCE.read_text()
        begin = source.index("void Tab5VisionService::TryGreeting(")
        end = source.index("void Tab5VisionService::Run()", begin)
        actual = source[begin:end]
        harness = r'''
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <utility>

constexpr int64_t kGreetingRequestLifetimeUs = 8LL * 1000000;
const unsigned char nabo_greeting_ogg[] = {'x'};
constexpr size_t nabo_greeting_ogg_len = sizeof(nabo_greeting_ogg);
std::atomic<int64_t> fake_now{1000000};
int64_t esp_timer_get_time() { return fake_now.load(); }
enum DeviceState { kDeviceStateIdle, kDeviceStateListening };
struct Display { int welcomes = 0; void WelcomeBack() { ++welcomes; } };
struct Application {
    bool external = false;
    DeviceState state = kDeviceStateIdle;
    int sounds = 0;
    std::deque<std::function<void()>> jobs;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()>&& job) { jobs.push_back(std::move(job)); }
    bool IsExternalAudioActive() const { return external; }
    DeviceState GetDeviceState() const { return state; }
    void PlaySound(std::string_view) { ++sounds; }
    void RunAll() {
        while (!jobs.empty()) {
            auto job = std::move(jobs.front());
            jobs.pop_front();
            job();
        }
    }
};
struct Tab5VisionService {
    Display* display_ = nullptr;
    std::atomic<bool> greeting_owed_{false};
    std::atomic<bool> greeting_queued_{false};
    std::atomic<int64_t> last_greeting_us_{0};
    void TryGreeting(int64_t detected_at_us);
};
''' + actual + r'''
int main() {
    Display display;
    Tab5VisionService vision;
    vision.display_ = &display;
    auto& app = Application::GetInstance();

    vision.greeting_owed_ = true;
    vision.TryGreeting(1000000);
    vision.TryGreeting(1000000);
    if (app.jobs.size() != 1) return 1;
    app.external = true;
    fake_now = 1100000;
    app.RunAll();
    if (app.sounds || !vision.greeting_owed_ || vision.greeting_queued_) return 2;

    app.external = false;
    vision.TryGreeting(1200000);
    fake_now = 1300000;
    app.RunAll();
    if (app.sounds != 1 || display.welcomes != 1 || vision.greeting_owed_ ||
        vision.last_greeting_us_ != 1300000) return 3;

    vision.greeting_owed_ = true;
    vision.TryGreeting(2000000);
    fake_now = 11000000;
    app.RunAll();
    if (app.sounds != 1 || !vision.greeting_owed_ || vision.greeting_queued_) return 4;
    vision.TryGreeting(11000000);
    fake_now = 11100000;
    app.RunAll();
    if (app.sounds != 2 || display.welcomes != 2) return 5;

    vision.greeting_owed_ = true;
    vision.TryGreeting(12000000);
    vision.greeting_owed_ = false;
    fake_now = 12100000;
    app.RunAll();
    if (app.sounds != 2 || vision.greeting_queued_) return 6;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "greeting.cc"
            binary = Path(directory) / "greeting"
            cpp.write_text(harness)
            compiled = subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
