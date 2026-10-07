"""Exercise the real Muse UI methods with a fake display and delayed inbox events."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/tab5_native_apps.cc"


class MuseDeferredUiTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_hidden_updates_are_deferred_and_open_marks_batch_read_once(self):
        source = SOURCE.read_text()
        same = source[source.index("bool SameMuseList("):
                      source.index("std::shared_ptr<LvglFont> MusicTextFontOwner()")]
        opening = source[source.index("void Tab5NativeApps::OpenMuse()"):
                         source.index("void Tab5NativeApps::OpenSettings()")]
        update = source[source.index("void Tab5NativeApps::SetMuseInbox("):
                        source.index("void Tab5NativeApps::BuildSettings()")]
        harness = r'''
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <memory>

namespace tab5_podcast { struct EpisodeList { bool empty() const { return true; } }; using SharedEpisodes = std::shared_ptr<const EpisodeList>; }
namespace tab5_muse {
struct Message {
    int id = 0;
    std::string title, body, from, time;
};
struct Snapshot {
    tab5_podcast::SharedEpisodes episodes;
    bool ok = false, ever_ok = false, podcast_ok = false;
    unsigned poll_count = 0;
    int latest_id = 0, seen_id = 0;
    std::string host, url, mcp_url;
    std::vector<Message> messages;
    int Unread() const {
        int count = 0;
        for (const auto& message : messages) count += message.id > seen_id;
        return count;
    }
};
}

struct FakePage { bool hidden = true; };
struct lv_font_t {};
lv_font_t current_font;
const lv_font_t* MusicTextFont() { return &current_font; }
constexpr int LV_OBJ_FLAG_HIDDEN = 1;
bool lv_obj_has_flag(FakePage* page, int) { return page->hidden; }

''' + same + r'''
class Tab5NativeApps {
public:
    struct Actions {
        std::function<void()> muse_refresh;
        std::function<void()> muse_opened;
    } actions_;
    FakePage page_;
    FakePage* muse_page_ = &page_;
    bool root_visible_ = false;
    bool muse_list_dirty_ = false;
    const lv_font_t* muse_rendered_font_ = nullptr;
    tab5_muse::Snapshot muse_snapshot_;
    std::string entry_, status_, url_;
    std::string *muse_entry_label_ = &entry_, *muse_status_ = &status_, *muse_url_ = &url_;
    int renders_ = 0;
    tab5_muse::Snapshot rendered_;
    std::vector<std::function<void()>> scheduled_;

    // Muse 电台 page is created lazily; these harnesses never open it.
    struct PodcastStub {
        void Tick() {}
        void SetPlayback(bool, const char*) {}
        void SetRefreshResult(bool, unsigned) {}
        template <typename T> void SetEpisodes(const T&) {}
    };
    PodcastStub* podcast_page_ = nullptr;
    tab5_podcast::SharedEpisodes podcast_episodes_;
    void UpdatePodcastEntry() {}
    bool IsVisible() const { return root_visible_; }
    void Show(FakePage* page) { root_visible_ = true; page->hidden = false; }
    void Close() { root_visible_ = false; page_.hidden = true; }
    void Schedule(std::function<void()> action) { scheduled_.push_back(std::move(action)); }
    static void SetLabelTextIfChanged(std::string* label, const char* text) {
        if (label && *label != text) *label = text;
    }
    void RenderMuseList(const tab5_muse::Snapshot& snapshot) {
        ++renders_;
        rendered_ = snapshot;
        muse_rendered_font_ = MusicTextFont();
    }
    void OpenMuse();
    void SetMuseInbox(const tab5_muse::Snapshot& snapshot);
};
''' + opening + update + r'''
int main() {
    Tab5NativeApps ui;
    tab5_muse::Snapshot first;
    first.ok = first.ever_ok = true;
    first.latest_id = 1;
    first.messages.push_back({1, "A", "first", "NAS", "today"});
    ui.SetMuseInbox(first);
    ui.SetMuseInbox(first);
    if (ui.renders_ != 0 || !ui.muse_list_dirty_ || ui.entry_.find("1") == std::string::npos)
        return 1;

    ui.OpenMuse();
    if (ui.renders_ != 1 || ui.rendered_.seen_id != 1 || ui.muse_list_dirty_ ||
        ui.entry_ != "打开推送" || ui.scheduled_.size() != 2)
        return 2;

    // The real mark-read callback should not rebuild the same list.
    first.seen_id = 1;
    ui.SetMuseInbox(first);
    if (ui.renders_ != 1) return 3;

    // In-place content edits with the same ID and count still refresh a visible page.
    first.messages[0].body = "edited";
    ui.SetMuseInbox(first);
    if (ui.renders_ != 2 || ui.rendered_.messages[0].body != "edited") return 4;

    ui.Close();
    auto second = first;
    second.latest_id = 2;
    second.messages.insert(second.messages.begin(), {2, "B", "new", "NAS", "now"});
    ui.SetMuseInbox(second);
    if (ui.renders_ != 2 || !ui.muse_list_dirty_) return 5;
    ui.OpenMuse();
    if (ui.renders_ != 3 || ui.rendered_.messages.size() != 2 || ui.rendered_.seen_id != 2)
        return 6;

    second.seen_id = 2;
    second.ok = false;
    ui.SetMuseInbox(second);
    if (ui.renders_ != 3 || ui.status_.find("无响应") == std::string::npos) return 7;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / "muse-deferred.cc"
            binary = Path(directory) / "muse-deferred"
            test.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
