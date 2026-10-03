"""Exercise the real daily-card page and deferred mirror methods with fake LVGL."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/tab5_native_display.h"


class DailyMirrorTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_flip_waits_for_render_and_stale_or_busy_updates_are_dropped(self):
        source = SOURCE.read_text()
        self.assertIn("self->OnDailyRenderReady();", source)
        self.assertIn("ShowDailyPage(NextDailyPage());\n        ApplyDailyMirrorAfterCardRender();",
                      source)
        self.assertIn("if (returning_home || daily_mirror_retry_on_home_)\n"
                      "            ShowDailyPage(daily_page_);", source)
        gate = source[source.index("    class DailyMirrorGate {"):
                      source.index("    } daily_mirror_gate_;") + len("    } daily_mirror_gate_;")]
        methods = source[source.index("    bool DailyPageValid(unsigned page) const {"):
                         source.index("    // Fill builtin trio")]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <string>

struct lv_obj_t {
    std::string text;
    uint32_t color = 0;
    int opacity = 0;
    bool hidden = false;
};
using lv_color_t = uint32_t;
constexpr int LV_OPA_COVER = 255;
constexpr int LV_OPA_30 = 77;
inline uint32_t lv_tick_get() { return 1000; }
inline lv_color_t lv_color_hex(uint32_t value) { return value; }
inline const char* lv_label_get_text(lv_obj_t* obj) { return obj->text.c_str(); }
inline void lv_label_set_text(lv_obj_t* obj, const char* value) { obj->text = value; }
inline void lv_obj_set_style_bg_color(lv_obj_t* obj, lv_color_t value, int) { obj->color = value; }
inline void lv_obj_set_style_text_color(lv_obj_t* obj, lv_color_t value, int) { obj->color = value; }
inline void lv_obj_set_style_bg_opa(lv_obj_t* obj, int value, int) { obj->opacity = value; }
namespace tab5_frame { constexpr int kDailyPage = 4; }
struct CauseGate {
    int marks = 0;
    void MarkOnce(int, uint32_t) { ++marks; }
};
struct FakeApps {
    bool visible = false;
    bool IsVisible() const { return visible; }
};

class QdtechTab5Display {
public:
    static constexpr unsigned kDailyPages = 9;
    lv_obj_t accent, title, body, prompt, message, dots[9];
    lv_obj_t* daily_accent_ = &accent;
    lv_obj_t* daily_title_label_ = &title;
    lv_obj_t* daily_body_label_ = &body;
    lv_obj_t* prompt_label_ = &prompt;
    lv_obj_t* message_label_ = &message;
    lv_obj_t* daily_dots_[9] = {};
    std::string daily_titles_[9], daily_bodies_[9], daily_sources_[9];
    std::string digest_prompt_ = "今日医学精选", digest_text_;
    unsigned daily_page_ = 3, next_page_tick_ = 0, tick_ = 0, digest_count_ = 0;
    uint64_t daily_page_revision_ = 0;
    int daily_date_key_ = -1;
    bool has_digest_ = false, active_ = false, speaking_ = false, music_active_ = false;
    CauseGate frame_cause_gate_;
    FakeApps apps;
    FakeApps* apps_ = &apps;

    QdtechTab5Display() {
        for (unsigned i = 0; i < kDailyPages; ++i) daily_dots_[i] = &dots[i];
        prompt.text = "随时倾听";
        message.text = "轻触下方按钮，开始对话。";
    }
    static void SetVisible(lv_obj_t* obj, bool visible) { obj->hidden = !visible; }
    static void SetLabelTextIfChanged(lv_obj_t* obj, const char* value) {
        if (obj->text != value) obj->text = value;
    }
''' + gate + r'''
    bool daily_mirror_retry_on_home_ = false;
''' + methods + r'''
};

int main() {
    QdtechTab5Display ui;
    ui.daily_date_key_ = 20261002;
    for (unsigned i = 6; i < 9; ++i) {
        ui.daily_titles_[i] = "临床" + std::to_string(i);
        ui.daily_bodies_[i] = "正文" + std::to_string(i);
        ui.daily_sources_[i] = "指南" + std::to_string(i);
    }
    ui.daily_titles_[3] = "每日一句";
    ui.daily_bodies_[3] = "一句话";

    ui.ShowDailyPage(6);
    assert(ui.title.text == "临床6" && ui.body.text == "正文6");
    assert(ui.prompt.text == "随时倾听" && ui.daily_mirror_gate_.HasPending());
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.prompt.text == "随时倾听");  // No render has completed yet.
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.prompt.text == "临床干货");
    assert(ui.message.text == "临床6\n正文6\n来源：指南6");
    assert(!ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(7);
    ui.ShowDailyPage(8);  // Replace a pending page; never queue two mirrors.
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.title.text == "临床8");
    assert(ui.message.text == "临床8\n正文8\n来源：指南8");
    assert(!ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(6);
    ui.active_ = true;
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    ui.active_ = false;
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.message.text == "临床8\n正文8\n来源：指南8");
    assert(!ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(7);
    ui.speaking_ = true;
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    ui.speaking_ = false;
    assert(!ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(6);
    ui.music_active_ = true;
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    ui.music_active_ = false;
    assert(!ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(7);
    ui.message.text = "新消息";  // An external UI update also owns the panel.
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.message.text == "新消息" && !ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(7);
    ++ui.daily_page_revision_;  // A newer revision must invalidate the pending mirror.
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.message.text == "新消息" && !ui.daily_mirror_gate_.HasPending());

    ui.ShowDailyPage(6);
    ui.ShowDailyPage(3);  // Quote page has no mirror; cancel the prior pearl.
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.title.text == "每日一句" && ui.message.text == "新消息");

    ui.apps.visible = true;
    ui.ShowDailyPage(6);  // A covered card must not mirror after an app frame.
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(!ui.daily_mirror_gate_.HasPending() && ui.daily_mirror_retry_on_home_);
    assert(ui.message.text == "新消息");
    ui.apps.visible = false;
    // Even if the app opens and closes between Tick calls, retry forces a replay.
    if (ui.daily_mirror_retry_on_home_) ui.ShowDailyPage(ui.daily_page_);
    assert(ui.daily_mirror_gate_.HasPending());
    assert(!ui.daily_mirror_retry_on_home_);
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.message.text == "新消息");
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.message.text == "临床6\n正文6\n来源：指南6");

    ui.ShowDailyPage(7);  // The app may open after a home card was queued.
    ui.apps.visible = true;
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(!ui.daily_mirror_gate_.HasPending() && ui.daily_mirror_retry_on_home_);
    assert(ui.message.text == "临床6\n正文6\n来源：指南6");
    ui.apps.visible = false;
    if (ui.daily_mirror_retry_on_home_) ui.ShowDailyPage(ui.daily_page_);
    ui.OnDailyRenderReady();
    ui.ApplyDailyMirrorAfterCardRender();
    assert(ui.message.text == "临床7\n正文7\n来源：指南7");
    ui.digest_count_ = 0;
    ui.daily_page_ = 8;
    assert(ui.NextDailyPage() == 3);  // Touch carousel still skips empty digest slots.
}
'''
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / "daily-mirror.cc"
            binary = Path(directory) / "daily-mirror"
            test.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
