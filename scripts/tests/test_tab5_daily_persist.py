"""Exercise the real daily-card persistence path with queued main-task writes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/tab5_native_display.h"


class DailyPersistTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_flash_write_is_deferred_and_snapshot_tracks_latest_day(self):
        source = SOURCE.read_text()
        methods = source[source.index("    void SetDailyCards("):
                         source.index("    // Called once the clock is known; restores today's cards")]
        harness = r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>

int display_lock_depth = 0;
bool on_main_task = false;
int writes = 0;
int commits = 0;
std::map<std::string, std::string> persisted;
std::function<void()> on_settings_open;

class QdtechTab5Display;
class DisplayLockGuard {
public:
    explicit DisplayLockGuard(QdtechTab5Display*) { ++display_lock_depth; }
    ~DisplayLockGuard() { --display_lock_depth; }
    bool locked() const { return true; }
};

class Application {
public:
    std::deque<std::function<void()>> tasks;
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    void Schedule(std::function<void()>&& callback) { tasks.push_back(std::move(callback)); }
    void Drain() {
        on_main_task = true;
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
        on_main_task = false;
    }
};

class Settings {
public:
    Settings(const char*, bool) {
        if (!on_main_task || display_lock_depth != 0) std::abort();
        if (on_settings_open) {
            auto callback = std::move(on_settings_open);
            callback();
        }
    }
    ~Settings() { ++commits; }
    void SetInt(const std::string& key, int value) {
        ++writes;
        persisted[key] = std::to_string(value);
    }
    void SetString(const std::string& key, const std::string& value) {
        ++writes;
        persisted[key] = value;
    }
};

class QdtechTab5Display {
public:
    struct DailyPersistSnapshot {
        int date_key = -1;
        std::string titles[3];
        std::string bodies[3];
    };
    std::atomic<bool> daily_persist_queued_{false};
    int daily_date_key_ = -1;
    std::string daily_titles_[9], daily_bodies_[9];
    unsigned digest_count_ = 0;
    bool has_digest_ = false;
    void ShowDailyPage(unsigned) {}
''' + methods + r'''
};

int main() {
    QdtechTab5Display ui;
    const char* first_titles[] = {"first", nullptr, nullptr};
    const char* first_bodies[] = {"one", nullptr, nullptr};
    const char* second_titles[] = {"second", nullptr, nullptr};
    const char* second_bodies[] = {"two", nullptr, nullptr};

    ui.daily_date_key_ = 20261001;
    ui.SetDailyCards(first_titles, first_bodies);
    ui.SetDailyCards(second_titles, second_bodies);
    if (writes != 0 || Application::GetInstance().tasks.size() != 1 ||
        ui.daily_titles_[0] != "second") return 1;
    Application::GetInstance().Drain();
    if (commits != 1 || writes != 7 || persisted["date"] != "20261001" ||
        persisted["t0"] != "second" || persisted["b0"] != "two") return 2;

    // Before clock sync, no undated card is written; the later date uses the same cards.
    ui.daily_date_key_ = -1;
    ui.SetDailyCards(first_titles, first_bodies);
    Application::GetInstance().Drain();
    if (commits != 1) return 3;
    ui.daily_date_key_ = 20261002;
    ui.QueueDailyPersist();
    Application::GetInstance().Drain();
    if (commits != 2 || persisted["date"] != "20261002" || persisted["t0"] != "first")
        return 4;

    // A queued write must read date and cards together after a midnight rollover.
    ui.daily_date_key_ = 20261002;
    ui.SetDailyCards(first_titles, first_bodies);
    ui.daily_date_key_ = 20261003;
    ui.daily_titles_[0].clear();
    ui.daily_bodies_[0].clear();
    ui.QueueDailyPersist();
    Application::GetInstance().Drain();
    if (commits != 3 || persisted["date"] != "20261003" || !persisted["t0"].empty())
        return 5;

    // If today's push arrives while an old snapshot is being written, the newer
    // callback remains queued and is the last commit.
    ui.daily_date_key_ = 20261003;
    ui.SetDailyCards(first_titles, first_bodies);
    on_settings_open = [&] {
        ui.daily_date_key_ = 20261004;
        ui.SetDailyCards(second_titles, second_bodies);
    };
    Application::GetInstance().Drain();
    if (commits != 5 || persisted["date"] != "20261004" ||
        persisted["t0"] != "second" || persisted["b0"] != "two" ||
        display_lock_depth != 0) return 6;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / "daily-persist.cc"
            binary = Path(directory) / "daily-persist"
            test.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
