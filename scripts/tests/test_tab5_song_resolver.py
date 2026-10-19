"""Exercise production song resolution and late favorite playback callbacks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/qdtech/tab5'
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'


def method(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which('c++') and shutil.which('cc') and CJSON.exists(),
                     'Host compilers and ESP-IDF cJSON dependency required')
class SongResolverTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp/'test.cc').write_text(source)
            subprocess.run(['cc','-c',str(CJSON/'cJSON.c'),'-o',str(temp/'json.o')],
                           check=True,capture_output=True,text=True)
            result = subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                                     '-I',str(BOARD),'-I',str(CJSON),str(temp/'test.cc'),
                                     str(temp/'json.o'),'-o',str(temp/'test')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(temp/'test')],check=True,capture_output=True,text=True,timeout=10)

    def test_encoded_query_optional_id_trial_errors_and_pending_replacement(self):
        source = (BOARD/'tab5_muse_inbox.cc').read_text()
        header = (BOARD/'tab5_muse_inbox.h').read_text()
        structs = header[header.index('struct ResolvedTrack {'):header.index('struct RelayConnection {')]
        methods = '\n'.join(method(source, name) for name in (
            'void Inbox::ResolveTrack(', 'void Inbox::ResolveSong(',
            'std::string SongQueryEscape(', 'void Inbox::ServeTrackRequest('))
        self.compile_run(r'''
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cJSON.h>
#include "nabo_song_favorites.h"
template<typename... T> void Log(const char*, T&&...) {}
#define ESP_LOGI(tag, ...) Log(__VA_ARGS__)
std::string request,body=R"({"ok":true,"title":"歌 & / ?","artist":"歌手 A / B","song_id":"123","url":"http://music.example/song.mp3","is_trial":true})";
int status=200;
std::string HttpGet(const std::string& url,int* code,size_t limit,int timeout) {
 assert(limit==8192 && timeout==20000);request=url;*code=status;return body;
}
std::string JsonString(cJSON* root,const char* key) {
 auto* item=cJSON_GetObjectItem(root,key);
 return cJSON_IsString(item)&&item->valuestring?item->valuestring:"";
}
''' + structs + r'''
class Inbox {
public:
 using TrackCallback=std::function<void(const ResolvedTrack&)>;
 std::mutex mutex_;
 struct {std::string url="https://relay.example/inbox/private",host="nas:8787";} snapshot_;
 bool track_pending_=false;
 int track_episode_=0,track_index_=0,wakes=0;
 std::string track_title_,track_artist_,track_song_id_;
 TrackCallback track_done_;
 void RequestRefresh() {++wakes;}
 void ResolveTrack(int,int,TrackCallback);
 void ResolveSong(const std::string&,const std::string&,const std::string&,TrackCallback);
 void ServeTrackRequest();
};
''' + methods + r'''
int main() {
 Inbox inbox;int calls=0,old_calls=0;ResolvedTrack result;
 auto done=[&](const auto& track){++calls;result=track;};
 inbox.ResolveSong("obsolete","","",[&](const auto&){++old_calls;});
 inbox.ResolveSong("歌 & / ?","歌手 A / B","123",done);
 inbox.ServeTrackRequest();assert(calls==1 && old_calls==0 && result.ok && result.is_trial);
 assert(request=="https://relay.example/podcast/private/song?title=%E6%AD%8C%20%26%20%2F%20%3F&artist=%E6%AD%8C%E6%89%8B%20A%20%2F%20B&id=123");
 inbox.ServeTrackRequest();assert(calls==1);
 // The older episode route must not inherit title or ID from a favorite.
 inbox.ResolveTrack(42,2,done);inbox.ServeTrackRequest();
 assert(result.ok && result.episode==42 && result.index==2);
 assert(request=="https://relay.example/podcast/private/track?episode=42&index=2");
 inbox.snapshot_.url.clear();
 inbox.ResolveSong("歌","歌手","",done);inbox.ServeTrackRequest();
 assert(result.ok && request.find("http://nas:8787/tab5/podcast/song?title=")==0 && request.find("&id=")==std::string::npos);
 inbox.ResolveSong("歌","歌手","456",done);inbox.ServeTrackRequest();assert(!result.ok);
 status=404;body=R"({"ok":false,"code":"id_mismatch"})";
 inbox.ResolveSong("歌","歌手","123",done);inbox.ServeTrackRequest();
 assert(!result.ok && result.http_status==404 && result.error_code=="id_mismatch");
 body="not found";inbox.ResolveSong("歌","歌手","",done);inbox.ServeTrackRequest();
 assert(!result.ok && result.http_status==404);
 status=502;inbox.ResolveSong("歌","歌手","",done);inbox.ServeTrackRequest();assert(!result.ok && result.http_status==502);
 status=200;body=R"({"ok":true,"title":"歌","url":"file:///etc/passwd","song_id":"123"})";
 inbox.ResolveSong("歌","歌手","",done);inbox.ServeTrackRequest();assert(!result.ok);
 body=R"({"ok":true,"title":"歌","url":"http://audio.example/x","song_id":"123x"})";
 inbox.ResolveSong("歌","歌手","",done);inbox.ServeTrackRequest();assert(!result.ok);
}
''')

    def test_stop_or_new_source_cancels_delayed_favorite_and_preserves_identity(self):
        source = (BOARD/'m5stack_tab5.cc').read_text()
        action = source[source.index('actions.podcast_favorite_play ='):source.index('actions.muse_refresh =',source.index('actions.podcast_favorite_play ='))]
        self.compile_run(r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <string>
#include <vector>
#include "nabo_song_favorites.h"
struct Application {
 static Application& GetInstance() {static Application app;return app;}
 void Schedule(std::function<void()> fn) {fn();}
};
namespace tab5_muse {
struct ResolvedTrack {
 bool ok=true,is_trial=false;int http_status=200;
 std::string title="resolved",artist="resolved artist",url="http://example/song",song_id="123";
};
struct Inbox {
 static Inbox& GetInstance() {static Inbox inbox;return inbox;}
 std::vector<std::function<void(const ResolvedTrack&)>> pending;
 void ResolveSong(const std::string& title,const std::string& artist,const std::string& id,
                  std::function<void(const ResolvedTrack&)> done) {
   assert(title=="收藏歌" && artist=="收藏歌手" && id=="123");pending.push_back(done);
 }
};
}
struct Tab5SongFavorites {
 static Tab5SongFavorites* Get() {static Tab5SongFavorites f;return &f;}
 int remembers=0;
 void Remember(const std::string& title,const std::string& artist,const std::string& id) {
  assert(title=="收藏歌" && artist=="收藏歌手" && id=="123");++remembers;
 }
};
struct QdtechTab5Display {bool trial=false;void SetPodcastSongTrial(bool on) {trial=on;}};
struct Board {
 bool podcast_session_=true;unsigned podcast_request_seq_=0;
 std::atomic<unsigned> music_request_generation_{0};std::atomic<bool> native_radio_ready_{true};
 struct {void Stop() {}} radio_service_;
 QdtechTab5Display display;void* display_=&display;
 std::string status;int plays=0;
 void ReplaceMusicSource(std::function<void()> fn) {++music_request_generation_;fn();}
 void EndContinuousSession() {}
 void SetPodcastStatus(const char* text) {status=text;}
 std::string PlayMusicRequest(const std::string&,const std::string&,const std::string&,
                             const std::string&,const std::string& id,bool continuous) {
  assert(id=="123" && !continuous);++plays;++music_request_generation_;return "started";
 }
 struct {std::function<bool(const std::string&,const std::string&,const std::string&)> podcast_favorite_play;} actions;
 void SetUp() {
''' + action + r'''
 }
};
int main() {
 Board b;b.SetUp();auto& inbox=tab5_muse::Inbox::GetInstance();
 assert(!b.actions.podcast_favorite_play("","artist","123"));
 assert(!b.actions.podcast_favorite_play("歌","artist","123x"));assert(inbox.pending.empty());
 auto play=[&]{assert(b.actions.podcast_favorite_play("收藏歌","收藏歌手","123"));};
 tab5_muse::ResolvedTrack track;
 play();assert(!b.podcast_session_);++b.podcast_request_seq_; // stop while resolving
 inbox.pending[0](track);assert(b.plays==0);
 play();++b.music_request_generation_; // another audio source owns the player
 inbox.pending[1](track);assert(b.plays==0);
 play();play();inbox.pending[2](track);assert(b.plays==0); // a newer favorite wins
 track.is_trial=true;inbox.pending[3](track);assert(b.plays==1 && b.display.trial);
 assert(Tab5SongFavorites::Get()->remembers==1);
 play();track.ok=false;track.http_status=404;inbox.pending[4](track);assert(b.status=="暂不可播" && b.plays==1);
 play();track.http_status=502;inbox.pending[5](track);assert(b.status=="获取失败，请稍后重试" && b.plays==1);
}
''')
