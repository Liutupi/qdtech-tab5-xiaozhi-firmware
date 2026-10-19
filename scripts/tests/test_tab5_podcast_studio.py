"""Exercise shared studio ownership across FM, podcast, and home navigation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


def method(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which("c++"), "Host C++ compiler required")
class PodcastStudioTests(unittest.TestCase):
    def test_switching_pages_keeps_one_lease_and_cancels_hidden_tuning(self):
        radio = (BOARD / "tab5_radio_page.cc").read_text()
        podcast = (BOARD / "tab5_podcast_page.cc").read_text()
        cpp = r'''
#include <cassert>
#include <functional>
#include <string>
#include "nabo_radio_mood.h"
#include "tab5_podcast_model.h"
unsigned now=100;
unsigned lv_tick_get() {return now;}
class Tab5PodcastPage {
public:
 enum class PlaybackKind {None,Podcast,Songs};
 PlaybackKind playback_kind_=PlaybackKind::Podcast;
 int playback_episode_id_=4,selected_id_=4,active_cue_=-1;
 tab5_podcast::EpisodeList episodes_;
 const auto& List() const {return episodes_;}
 nabo_radio::Mood StudioMood(const char*) const;
};
struct Stage {
 int starts=0,stops=0,ticks=0;
 bool active=false;
 nabo_radio::Mood last;
 void SetActive(bool on) {active=on;on?++starts:++stops;}
 void Tick(unsigned,const nabo_radio::Mood& mood,int level) {
   assert(active && level==37);last=mood;++ticks;
 }
};
class Tab5NativeApps {
public:
 bool radio=false,podcast=false,radio_stage_active_=false,dial_dragging_=false;
 int tune_index_=-1,radio_current_=-1,tune_sent_index_=-1;
 unsigned tune_due_ms_=0,tune_sent_ms_=0;
 std::string radio_link_state_="Playing";
 Stage stage;
 Stage* radio_stage_=&stage;
 Tab5PodcastPage page;
 Tab5PodcastPage* podcast_page_=&page;
 int selected_calls=0;
 void* radio_state_=nullptr;
 struct Actions {
   std::function<int()> radio_level=[] {return 37;};
   std::function<void(int)> radio_select;
 } actions_;
 bool IsRadioVisible() const {return radio;}
 bool IsPodcastVisible() const {return podcast;}
 nabo_radio::Mood RadioMood() const {return {nabo_radio::Link::Playing,true};}
 static void SetLabelTextIfChanged(void*,const char*) {}
 static void Schedule(std::function<void()> fn) {fn();}
 void TickRadio();
};
''' + method(podcast, "nabo_radio::Mood Tab5PodcastPage::StudioMood(") + '\n' + method(radio, "void Tab5NativeApps::TickRadio()") + r'''
int main() {
 Tab5NativeApps ui;
 tab5_podcast::Episode episode;episode.id=4;
 episode.cues={{0,1000,"旁白",false},{1000,2000,"音乐",true}};
 ui.page.episodes_.push_back(episode);
 ui.actions_.radio_select=[&](int){++ui.selected_calls;};
 ui.TickRadio();assert(ui.stage.starts==0 && ui.stage.ticks==0);
 ui.podcast=true;ui.TickRadio();
 assert(ui.stage.starts==1 && !ui.stage.last.music && ui.stage.last.link==nabo_radio::Link::Playing);
 ui.page.active_cue_=1;ui.TickRadio();assert(ui.stage.last.music && ui.stage.starts==1);
 ui.page.playback_kind_=Tab5PodcastPage::PlaybackKind::Songs;
 ui.radio_link_state_="Buffering";ui.TickRadio();
 assert(ui.stage.last.music && ui.stage.last.link==nabo_radio::Link::Connecting);
 ui.podcast=false;ui.radio=true;ui.TickRadio();
 assert(ui.stage.starts==1 && ui.stage.stops==0 && ui.stage.last.music);
 ui.tune_due_ms_=500;ui.tune_index_=3;ui.dial_dragging_=true;
 ui.radio=false;ui.podcast=true;ui.TickRadio();
 assert(ui.tune_due_ms_==0 && ui.tune_index_==-1 && !ui.dial_dragging_);
 now=1000;ui.TickRadio();assert(ui.selected_calls==0 && ui.stage.starts==1);
 ui.podcast=false;ui.TickRadio();ui.TickRadio();
 assert(ui.stage.stops==1 && !ui.stage.active);
 ui.radio=true;ui.TickRadio();assert(ui.stage.starts==2 && ui.stage.stops==1);
 ui.page.playback_kind_=Tab5PodcastPage::PlaybackKind::Podcast;
 ui.page.selected_id_=5;
 assert(!ui.page.StudioMood("Playing").music); // never use another episode's cue
 assert(ui.page.StudioMood("Waiting WiFi").link==nabo_radio::Link::NoNetwork);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "studio.cc").write_text(cpp)
            result = subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                                     str(path / "studio.cc"), "-o", str(path / "studio")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / "studio")], check=True, capture_output=True, text=True)
