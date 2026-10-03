"""Exercise the Tab5 radio's real font switch without an ESP-IDF build."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/tab5_native_apps.cc"


class RadioFontSyncTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_radio_and_muse_keep_fonts_alive_through_theme_replacement(self):
        source = SOURCE.read_text()
        build_radio = source[source.index("void Tab5NativeApps::BuildRadio()"):
                             source.index("void Tab5NativeApps::DrawWave(")]
        self.assertNotIn("SyncRadioTextFont()", build_radio)
        font_selector = source[source.index("std::shared_ptr<LvglFont> MusicTextFontOwner()"):
                               source.index("}  // namespace")]
        open_radio = source[source.index("void Tab5NativeApps::OpenRadio("):
                            source.index("void Tab5NativeApps::OpenNes()")]
        open_muse = source[source.index("void Tab5NativeApps::OpenMuse()"):
                           source.index("void Tab5NativeApps::OpenSettings()")]
        tick = source[source.index("void Tab5NativeApps::Tick() {"):
                      source.index("    if (IsVisible() && muse_page_", source.index(
                          "void Tab5NativeApps::Tick() {"))] + "}\n"
        sync = source[source.index("void Tab5NativeApps::SyncRadioTextFont()"):
                      source.index("void Tab5NativeApps::SetMusicLyricsWindow(")]
        muse_begin = source.index("void Tab5NativeApps::RenderMuseList(")
        muse_prefix = source[muse_begin:source.index(
            "    if (snapshot.messages.empty())", muse_begin)]
        muse_prefix = muse_prefix.replace("{\n", "{\n    (void)snapshot;\n", 1) + "}\n"
        harness = r'''
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>

struct lv_font_t { int line_height; };
struct lv_obj_t {
    const lv_font_t* font = nullptr;
    const char* text = "";
    int style_updates = 0;
};
lv_font_t qd_font_cjk_28{30};
lv_font_t full_font{43};
lv_font_t second_font{43};
lv_font_t third_font{43};
lv_font_t fourth_font{43};
lv_font_t muse_font{43};
void lv_obj_set_style_text_font(lv_obj_t* label, const lv_font_t* font, int) {
    label->font = font;
    ++label->style_updates;
}
const char* lv_label_get_text(lv_obj_t* label) { return label->text; }

struct FakeFont {
    const lv_font_t* selected = nullptr;
    const lv_font_t* font() const { return selected; }
};
using LvglFont = FakeFont;
std::weak_ptr<FakeFont> old_muse_font;
bool check_old_muse_font = false;
void lv_obj_clean(lv_obj_t*) {
    if (check_old_muse_font && old_muse_font.expired()) __builtin_trap();
}
namespace tab5_muse {
struct Snapshot { unsigned latest_id = 0; unsigned seen_id = 0; };
}
struct FakeTheme {
    std::shared_ptr<FakeFont> selected;
    std::shared_ptr<FakeFont> GetTextFont() const { return selected; }
};
struct LvglThemeManager {
    FakeTheme* theme = nullptr;
    static LvglThemeManager& GetInstance() {
        static LvglThemeManager manager;
        return manager;
    }
    FakeTheme* GetTheme(const char*) { return theme; }
};
''' + font_selector + r'''
class Tab5NativeApps {
public:
    static constexpr int kRows = 6;
    const lv_font_t* music_font_ = nullptr;
    std::shared_ptr<LvglFont> music_font_owner_;
    const lv_font_t* muse_rendered_font_ = nullptr;
    std::shared_ptr<LvglFont> muse_font_owner_;
    lv_obj_t list;
    lv_obj_t* muse_list_ = &list;
    lv_obj_t muse_page;
    lv_obj_t* muse_page_ = &muse_page;
    lv_obj_t* muse_entry_label_ = nullptr;
    tab5_muse::Snapshot muse_snapshot_;
    bool muse_list_dirty_ = false;
    lv_obj_t title, lyric, rows[kRows];
    lv_obj_t page;
    lv_obj_t* radio_page_ = &page;
    lv_obj_t* radio_station_ = &title;
    lv_obj_t* radio_lyric_previous_ = nullptr;
    lv_obj_t* radio_lyric_ = &lyric;
    lv_obj_t* radio_lyric_next_ = nullptr;
    lv_obj_t* station_names_[kRows] = {&rows[0], &rows[1], &rows[2],
                                      &rows[3], &rows[4], &rows[5]};
    int lyric_layouts = 0;
    int wave_updates = 0;
    int shown = 0;
    int refreshed = 0;
    const lv_font_t* expected_font = &full_font;
    struct Actions {
        std::function<void()> start_radio;
        std::function<void()> muse_refresh;
        std::function<void()> muse_opened;
    } actions_;
    bool radio_visible = false;
    bool IsRadioVisible() const { return radio_visible; }
    void Show(lv_obj_t* page) {
        if (page == muse_page_) {
            if (muse_rendered_font_ != expected_font) __builtin_trap();
            ++shown;
            return;
        }
        if (music_font_ != expected_font || title.font != expected_font ||
            lyric.font != expected_font) __builtin_trap();
        for (auto& row : rows)
            if (row.font != expected_font) __builtin_trap();
        radio_visible = true;
        ++shown;
    }
    void RefreshStations() { ++refreshed; }
    static void SetLabelTextIfChanged(lv_obj_t*, const char*) {}
    static void Schedule(std::function<void()> action) { if (action) action(); }
    void UpdateWave() { ++wave_updates; }
    void SetMusicLyricLine(const char* text) {
        if (std::strcmp(text, radio_lyric_->text) != 0) __builtin_trap();
        ++lyric_layouts;
    }
    void SyncRadioTextFont();
    void RenderMuseList(const tab5_muse::Snapshot& snapshot);
    void OpenMuse();
    void OpenRadio(bool start_playback = true);
    void Tick();
};
''' + sync + tick + open_radio + open_muse + muse_prefix + r'''
int main() {
    Tab5NativeApps ui;
    ui.lyric.text = "醒来看看";
    ui.title.font = &qd_font_cjk_28;
    ui.lyric.font = &qd_font_cjk_28;
    for (auto& row : ui.rows) row.font = &qd_font_cjk_28;

    FakeTheme theme;
    theme.selected = std::make_shared<FakeFont>();
    theme.selected->selected = &full_font;
    LvglThemeManager::GetInstance().theme = &theme;
    ui.Tick();
    if (ui.music_font_ != nullptr || ui.title.style_updates != 0 ||
        ui.wave_updates != 1) return 1;
    ui.OpenRadio(false);
    if (ui.shown != 1 || ui.refreshed != 1 || ui.music_font_ != &full_font ||
        ui.title.style_updates != 1 || ui.lyric.style_updates != 1 ||
        ui.lyric_layouts != 1) return 2;
    for (auto& row : ui.rows)
        if (row.font != &full_font || row.style_updates != 1) return 3;
    std::weak_ptr<FakeFont> old_radio_font = theme.selected;
    theme.selected = std::make_shared<FakeFont>();
    theme.selected->selected = &second_font;
    ui.radio_visible = false;
    ui.Tick();
    if (old_radio_font.expired() || ui.music_font_ != &full_font) return 4;
    ui.expected_font = &second_font;
    ui.OpenRadio(false);
    if (!old_radio_font.expired() || ui.shown != 2 || ui.refreshed != 2 ||
        ui.music_font_ != &second_font || ui.title.style_updates != 2) return 5;

    old_radio_font = theme.selected;
    theme.selected = std::make_shared<FakeFont>();
    theme.selected->selected = &third_font;
    if (old_radio_font.expired()) return 6;
    ui.Tick();
    if (!old_radio_font.expired() || ui.music_font_ != &third_font ||
        ui.title.font != &third_font || ui.title.style_updates != 3 ||
        ui.lyric.font != &third_font || ui.lyric_layouts != 3 ||
        ui.wave_updates != 3) return 7;

    theme.selected = std::make_shared<FakeFont>();
    theme.selected->selected = &muse_font;
    Tab5NativeApps inbox;
    inbox.expected_font = &muse_font;
    inbox.OpenMuse();
    if (inbox.shown != 1 || inbox.muse_rendered_font_ != &muse_font) return 8;
    std::weak_ptr<FakeFont> old_inbox_font = theme.selected;
    theme.selected = std::make_shared<FakeFont>();
    theme.selected->selected = &fourth_font;
    if (old_inbox_font.expired()) return 9;
    inbox.expected_font = &fourth_font;
    old_muse_font = old_inbox_font;
    check_old_muse_font = true;
    inbox.OpenMuse();
    check_old_muse_font = false;
    if (!old_inbox_font.expired() || inbox.muse_rendered_font_ != &fourth_font ||
        inbox.shown != 2) return 10;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "radio_font_sync.cc"
            binary = Path(directory) / "radio_font_sync"
            cpp.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
