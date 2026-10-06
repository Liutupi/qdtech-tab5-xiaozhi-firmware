"""Exercise playback time, untrusted transcript parsing, and the real LVGL update handler."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/qdtech/tab5'
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'


@unittest.skipUnless(shutil.which('c++') and shutil.which('cc') and CJSON.exists(),
                     'Host compilers and cJSON required')
class PodcastTranscriptTests(unittest.TestCase):
    def test_actual_audio_progress_reconnect_stale_cues_and_manual_scroll(self):
        source = (BOARD / 'tab5_podcast_page.cc').read_text()
        start = source.index('void Tab5PodcastPage::UpdateTranscript()')
        handler = source[start:source.index('\nvoid Tab5PodcastPage::DrawWave', start)]
        harness = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <string_view>
#include <vector>
#include "tab5_playback_clock.h"
#include "tab5_podcast_transcript.h"
struct lv_obj_t { unsigned color=0,opacity=0; int scrolls=0; };
unsigned now=100;
unsigned lv_tick_get() {return now;}
unsigned lv_color_hex(unsigned color) {return color;}
void lv_obj_set_style_text_color(lv_obj_t* row,unsigned color,int) {row->color=color;}
void lv_obj_set_style_bg_opa(lv_obj_t* row,unsigned opacity,int) {row->opacity=opacity;}
lv_obj_t* scroll_target=nullptr;
void lv_obj_update_layout(lv_obj_t*) {}
int lv_obj_get_y(lv_obj_t* row) {scroll_target=row;return 0;}
void lv_obj_scroll_to_y(lv_obj_t*,int,int) {++scroll_target->scrolls;}
constexpr unsigned kBody=1,kPink=2,LV_OPA_TRANSP=0,LV_OPA_COVER=255,LV_ANIM_ON=1;
struct Tab5PodcastPage {
 struct Callbacks {std::function<tab5_playback::Position(std::string_view)> playback_position;} callbacks_;
 enum class PlaybackKind {None,Podcast,Songs};
 PlaybackKind playback_kind_=PlaybackKind::Podcast;
 bool playing_=true,manual_scroll_=false,transcript_scroll_pending_=false;
 unsigned manual_scroll_started_=0;
 int active_cue_=-1,playback_episode_id_=4,selected_id_=4;
 std::vector<lv_obj_t*> cue_rows_;
 lv_obj_t* script_box_=nullptr;
 tab5_podcast::EpisodeList episodes_;
 const auto& List() const {return episodes_;}
 void UpdateTranscript();
};
''' + handler + r'''
int main() {
 tab5_playback::Clock clock;
 clock.Begin("https://relay/audio.mp3",5);
 assert(clock.Read("https://relay/audio.mp3",5).matches);
 assert(!clock.Read("https://relay/other.mp3",5).matches);
 assert(!clock.Read("https://relay/audio.mp3",6).matches);
 clock.Advance(24000,24000,5);
 assert(clock.Read("https://relay/audio.mp3",5).milliseconds==1000);
 clock.Advance(24000,24000,4); // stale decoder may still finish a frame
 clock.Advance(0,24000,5); clock.Advance(10,0,5);
 assert(clock.Read("https://relay/audio.mp3",5).milliseconds==1000);
 clock.Begin("https://relay/audio.mp3",5); // reconnect starts audio from the beginning
 assert(clock.Read("https://relay/audio.mp3",5).milliseconds==0);
 auto* root=cJSON_Parse(R"({"transcript":{"audio_filename":"audio.mp3","cues":[
 {"start_ms":0,"end_ms":900,"text":"早上好。"},
 {"start_ms":1100,"end_ms":2200,"text":"欢迎收听。"}]}})");
 assert(root);
 const auto cues=tab5_podcast::ParseTranscript(root,"https://relay/audio.mp3");
 assert(cues.size()==2);
 assert(tab5_podcast::ParseTranscript(root,"https://relay/replaced.mp3").empty());
 assert(tab5_podcast::ActiveCue(cues,899)==0);
 assert(tab5_podcast::ActiveCue(cues,900)==-1);
 assert(tab5_podcast::ActiveCue(cues,1100)==1);
 assert(tab5_podcast::ActiveCue(cues,2200)==-1);
 auto* items=cJSON_GetObjectItem(cJSON_GetObjectItem(root,"transcript"),"cues");
 auto* second=cJSON_GetArrayItem(items,1);
 cJSON_SetNumberValue(cJSON_GetObjectItem(second,"start_ms"),850);
 assert(tab5_podcast::ParseTranscript(root,"https://relay/audio.mp3").empty());
 cJSON_SetNumberValue(cJSON_GetObjectItem(second,"start_ms"),1100.5);
 assert(tab5_podcast::ParseTranscript(root,"https://relay/audio.mp3").empty());
 cJSON_Delete(root);
 Tab5PodcastPage page;
 tab5_podcast::Episode episode;episode.id=4;episode.audio_url="https://relay/audio.mp3";episode.cues=cues;
 page.episodes_.push_back(episode);
 lv_obj_t first,second_row;page.cue_rows_={&first,&second_row};
 unsigned generation=5;
 page.callbacks_.playback_position=[&](auto url){return clock.Read(url,generation);};
 page.UpdateTranscript();assert(page.active_cue_==0 && first.color==kPink && first.scrolls==1);
 // Buffering does not output PCM: wall time alone must not move text.
 now+=20000;page.UpdateTranscript();assert(page.active_cue_==0 && first.scrolls==1);
 page.manual_scroll_=true;page.manual_scroll_started_=now;
 clock.Advance(26400,24000,5);page.UpdateTranscript();
 assert(page.active_cue_==1 && second_row.scrolls==0 && first.color==kBody);
 now+=7999;page.UpdateTranscript();assert(second_row.scrolls==0);
 ++now;page.UpdateTranscript();assert(second_row.scrolls==1);
 clock.Begin("https://relay/audio.mp3",5);page.UpdateTranscript();assert(page.active_cue_==0);
 generation=6;page.UpdateTranscript();assert(page.active_cue_==-1 && first.color==kBody);
 generation=5;page.playback_kind_=Tab5PodcastPage::PlaybackKind::Songs;
 page.UpdateTranscript();assert(page.active_cue_==-1);
 page.playback_kind_=Tab5PodcastPage::PlaybackKind::Podcast;page.selected_id_=3;
 page.UpdateTranscript();assert(page.active_cue_==-1);
 auto different=page.episodes_;different[0].cues[0].end_ms=800;
 assert(!tab5_podcast::SameEpisodes(page.episodes_,different));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            cpp, obj, executable = temp/'transcript.cc', temp/'cjson.o', temp/'transcript'
            cpp.write_text(harness)
            subprocess.run(['cc', '-c', str(CJSON/'cJSON.c'), '-o', str(obj)], check=True,
                           capture_output=True, text=True)
            compiled = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(BOARD), '-I', str(CJSON), str(cpp), str(obj), '-o', str(executable)],
                           capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
