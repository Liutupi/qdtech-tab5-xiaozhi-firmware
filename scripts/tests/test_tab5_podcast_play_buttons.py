"""Exercise the real Muse page handlers and delayed episode-audio updates."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"


def method(source, name):
    start = source.index("void Tab5PodcastPage::" + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
class PodcastPlayButtonsTests(unittest.TestCase):
    def test_buttons_keep_narration_and_recommended_tracks_separate(self):
        source = (BOARD / "tab5_podcast_page.cc").read_text()
        handlers = "\n".join(method(source, name) for name in
                             ["PlaySelected", "PlaySongs", "PlayTrack", "UpdatePlayLabels", "Refresh", "SetRefreshResult"])
        harness = r'''
#include <algorithm>
#include <cassert>
#include <functional>
#include <string>
#include "tab5_podcast_model.h"
struct lv_obj_t { std::string text; };
void lv_label_set_text(lv_obj_t* obj, const char* text) { obj->text = text; }
constexpr unsigned kMuted=1, kPink=2, kViolet=3;
unsigned lv_tick_get() { return 100; }
struct Tab5PodcastPage {
  struct Callbacks {
    std::function<bool(const std::string&,const std::string&)> play_url;
    std::function<bool(int,int,int)> play_track;
    std::function<void()> stop;
    std::function<void()> refresh;
  } callbacks_;
  enum class PlaybackKind {None, Podcast, Songs};
  PlaybackKind playback_kind_=PlaybackKind::None;
  int playback_episode_id_=0, selected_id_=3;
  bool playing_=false;
  tab5_podcast::SharedEpisodes episodes_;
  lv_obj_t podcast_label, songs_label;
  lv_obj_t *play_label_=&podcast_label, *songs_label_=&songs_label;
  lv_obj_t refresh_label;
  lv_obj_t* refresh_label_=&refresh_label;
  bool refreshing_=false;
  unsigned last_poll_count_=0,refresh_started_=0;
  std::string status;
  const tab5_podcast::EpisodeList& List() const {return *episodes_;}
  void SetStatus(const char* text,unsigned) {status=text;}
  void Refresh(); void SetRefreshResult(bool,unsigned);
  void PlaySelected(); void PlaySongs(); void PlayTrack(size_t); void UpdatePlayLabels();
};
'''
        harness += handlers + r'''
int main() {
  auto list=tab5_podcast::MakeEpisodes();
  tab5_podcast::Episode episode; episode.id=3;episode.title="本期节目";
  episode.script="完整节目稿\n没有音频时不能把歌曲当播客";
  episode.tracks={{"曲一","歌手一"},{"曲二","歌手二"}};
  list->push_back(episode);
  Tab5PodcastPage page;page.episodes_=list;
  int urls=0,tracks=0,stops=0,index=-1;
  page.callbacks_.play_url=[&](const auto& url,const auto& title){
    assert(url=="https://relay.example/episode.mp3" && title=="本期节目");++urls;return true;};
  page.callbacks_.play_track=[&](int id,int i,int count){
    assert(id==3 && count==2);++tracks;index=i;return true;};
  page.callbacks_.stop=[&]{++stops;};
  int refreshes=0;
  page.callbacks_.refresh=[&]{++refreshes;};
  page.SetRefreshResult(true,5);
  page.Refresh();page.Refresh();
  assert(refreshes==1 && page.refresh_label.text=="刷新中…");
  page.SetRefreshResult(true,5);assert(page.refreshing_);
  page.SetRefreshResult(true,6);
  assert(!page.refreshing_ && page.refresh_label.text=="刷新完成");
  page.Refresh();page.SetRefreshResult(false,7);
  assert(refreshes==2 && page.refresh_label.text=="刷新失败");
  page.playing_=true;page.status="播放中";
  page.Refresh();page.SetRefreshResult(true,8);
  assert(page.status=="播放中");page.playing_=false;

  page.PlaySelected();
  assert(urls==0 && tracks==0 && page.status.find("尚未上传")!=std::string::npos);
  page.PlaySongs(); assert(tracks==1 && index==0 && urls==0);
  auto updated=tab5_podcast::MakeEpisodes();*updated=*list;
  assert(tab5_podcast::SameEpisodes(*list,*updated));
  (*updated)[0].audio_url="https://relay.example/episode.mp3";
  assert(!tab5_podcast::SameEpisodes(*list,*updated));
  assert((*list)[0].audio_url.empty() && (*updated)[0].tracks.size()==2);
  page.episodes_=updated;
  page.PlaySelected();assert(urls==1 && tracks==1);
  page.playing_=true;page.UpdatePlayLabels();
  assert(page.podcast_label.text=="停止播客" && page.songs_label.text=="播放推荐歌曲");
  page.PlaySelected();assert(stops==1 && urls==1);
  page.PlaySongs();assert(stops==1 && tracks==2 && index==0);
  assert(page.podcast_label.text=="播放播客" && page.songs_label.text=="停止推荐歌曲");
  page.PlaySongs();assert(stops==2 && tracks==2);
  page.PlaySelected();assert(urls==2 && tracks==2 && stops==2);
  page.PlayTrack(1);assert(tracks==3 && index==1);
  page.PlayTrack(2);assert(tracks==3);
  page.playing_=false;page.UpdatePlayLabels();
  assert(page.podcast_label.text=="播放播客" && page.songs_label.text=="播放推荐歌曲");
  updated=tab5_podcast::MakeEpisodes();*updated=*list;
  (*updated)[0].tracks[1].artist="更正歌手";
  assert(!tab5_podcast::SameEpisodes(*list,*updated));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "buttons.cc"
            executable = Path(directory) / "buttons"
            cpp.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-I", str(BOARD), str(cpp),
                            "-o", str(executable)], check=True, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)
