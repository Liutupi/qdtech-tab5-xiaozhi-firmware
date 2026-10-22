"""Replay the daily-music/podcast-refresh overlap with the real fetch and poll loop."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"


@unittest.skipUnless(shutil.which("c++") and shutil.which("cc") and CJSON.exists(),
                     "Host compilers and cJSON required")
class PodcastMemoryTests(unittest.TestCase):
    def test_playback_low_memory_and_inflight_refresh_keep_existing_data(self):
        source = (BOARD / "tab5_muse_inbox.cc").read_text()
        gate = source[source.index("bool CanRefreshPodcasts("):
                      source.index("std::string HttpGetDirect(")]
        fetch = source[source.index("bool Inbox::FetchPodcasts("):
                       source.index("bool Inbox::Poll()")]
        run = source[source.index("void Inbox::Run()"):
                     source.index("bool Inbox::PollMusic()")]
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cJSON.h>
#include "tab5_podcast_transcript.h"
#define ESP_LOGW(...) ((void)0)
constexpr size_t kMaxPodcastBytes=192*1024;
constexpr int kPodcastLimit=6,kPollSeconds=60,kFirstPollDelayMs=20000,kMusicPollMs=4000;
constexpr int MALLOC_CAP_SPIRAM=1;
size_t free_psram=2*1024*1024, largest_psram=1024*1024;
size_t heap_caps_get_free_size(int) {return free_psram;}
size_t heap_caps_get_largest_free_block(int) {return largest_psram;}
struct Application {
 bool active=false;
 static Application& GetInstance() {static Application app;return app;}
 bool IsExternalAudioActive() const {return active;}
};
int http_calls=0,json_allocs=0;
bool start_during_http=false,pressure_during_parse=false;
int status_code=200;
std::string response=R"({"episodes":[{"id":1,"title":"Daily radio","script":"A long script that requires a heap allocation","from":"Muse","time":"07:30","audio_url":"https://relay/a.mp3","tracks":[{"title":"A long song title","artist":"Artist"}],"transcript":{"audio_filename":"a.mp3","cues":[{"start_ms":0,"end_ms":2000,"text":"Opening narration","kind":"speech"}]}},{"id":2,"title":"Earlier radio"}]})";
std::string HttpGet(const std::string&,int* status,size_t max_bytes) {
 assert(max_bytes==kMaxPodcastBytes);++http_calls;*status=status_code;
 if(start_during_http)Application::GetInstance().active=true;
 return response;
}
void* JsonAlloc(size_t n) {
 if(pressure_during_parse)free_psram=128*1024;
 void* p=std::malloc(n);if(p)++json_allocs;return p;
}
void JsonFree(void* p) {if(p)--json_allocs;std::free(p);}
''' + gate + r'''
int loop_count=0,music_polls=0,full_polls=0,lookups=0;
int64_t now_us=0;
int64_t esp_timer_get_time() {return now_us;}
constexpr int pdTRUE=1;
int pdMS_TO_TICKS(int value) {return value;}
int ulTaskNotifyTake(int,int delay) {
 if(delay==kFirstPollDelayMs)return 0;
 ++loop_count;now_us+=delay*1000;
 // Playback and fragmentation defer a pending refresh; stopping retries it.
 if(loop_count==2)Application::GetInstance().active=false;
 if(loop_count==3)largest_psram=1024*1024;
 if(loop_count==4)throw 42;
 return 0;
}
namespace tab5_muse {
class Inbox {
 public:
 std::mutex mutex_;
 bool track_pending_=false,refresh_requested_=false;
 void Run();
 bool FetchPodcasts(const std::string&,tab5_podcast::EpisodeList*);
 void ServeTrackRequest() {++lookups;}
 bool PollMusic() {++music_polls;return true;}
 bool Poll() {++full_polls;assert(!Application::GetInstance().active);return true;}
};
''' + fetch + run + r'''
}
int main() {
 cJSON_Hooks hooks{JsonAlloc,JsonFree};cJSON_InitHooks(&hooks);
 tab5_muse::Inbox inbox;
 tab5_podcast::EpisodeList out;
 Application::GetInstance().active=true;
 assert(!inbox.FetchPodcasts("test",&out) && http_calls==0 && out.empty());
 Application::GetInstance().active=false;
 free_psram=128*1024;
 assert(!inbox.FetchPodcasts("test",&out) && http_calls==0);
 free_psram=2*1024*1024;largest_psram=64*1024;
 assert(!inbox.FetchPodcasts("test",&out) && http_calls==0);
 largest_psram=1024*1024;start_during_http=true;
 assert(!inbox.FetchPodcasts("test",&out) && http_calls==1 && json_allocs==0);
 Application::GetInstance().active=false;start_during_http=false;
 pressure_during_parse=true;
 assert(!inbox.FetchPodcasts("test",&out) && http_calls==2 && out.empty());
 assert(json_allocs==0);
 pressure_during_parse=false;free_psram=2*1024*1024;
 assert(inbox.FetchPodcasts("test",&out) && http_calls==3 && out.size()==2);
 assert(json_allocs==0 && out[0].script=="A long script that requires a heap allocation");
 assert(out[0].tracks.size()==1 && out[0].cues.size()==1);
 assert(out[0].cues[0].text=="Opening narration");
 // A failed refresh keeps the last valid list intact.
 auto cached=out;out.clear();status_code=503;
 assert(!inbox.FetchPodcasts("test",&out) && out.empty() && json_allocs==0);
 assert(cached[0].title=="Daily radio");
 status_code=200;response="invalid";
 assert(!inbox.FetchPodcasts("test",&out) && out.empty() && json_allocs==0);
 // Music command/track polling continues every iteration while the large poll waits.
 Application::GetInstance().active=true;largest_psram=64*1024;
 inbox.refresh_requested_=true;
 try {inbox.Run();assert(false);}catch(int value){assert(value==42);}
 assert(loop_count==4 && music_polls==4 && lookups==4 && full_polls==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            cpp, obj, exe = temp / "memory.cc", temp / "cjson.o", temp / "memory"
            cpp.write_text(harness)
            subprocess.run(["cc", "-c", str(CJSON / "cJSON.c"), "-o", str(obj)],
                           check=True, capture_output=True, text=True)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            "-I", str(CJSON), str(cpp), str(obj), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
