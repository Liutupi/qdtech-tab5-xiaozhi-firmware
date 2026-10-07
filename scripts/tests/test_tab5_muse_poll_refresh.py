"""Real poll regression: delayed audio updates must reach the UI independently of inbox messages."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/qdtech/tab5'
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'


@unittest.skipUnless(shutil.which('c++') and shutil.which('cc') and CJSON.exists(),
                     'Host compilers and ESP-IDF cJSON dependency required')
class MusePollRefreshTests(unittest.TestCase):
    def test_same_episode_audio_arrives_without_inbox_changes_and_failed_refresh_keeps_cache(self):
        source = (BOARD / 'tab5_muse_inbox.cc').read_text()
        poll = source[source.index('bool Inbox::Poll()'):source.index('\n}  // namespace tab5_muse')]
        header = (BOARD / 'tab5_muse_inbox.h').read_text()
        structs = header[header.index('struct Message {'):header.index('class Inbox {')]
        harness = r'''
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cJSON.h>
#include "tab5_podcast_model.h"
template<typename... T> void IgnoreLog(const char*, T&&...) {}
#define ESP_LOGI(tag, ...) IgnoreLog(__VA_ARGS__)
#define ESP_LOGW(...) ((void)0)
int json_allocations=0;
void* JsonAlloc(size_t size) { void* p=std::malloc(size);if(p)++json_allocations;return p; }
void JsonFree(void* p) { if(p)--json_allocations;std::free(p); }
std::string body=R"({"messages":[{"id":1,"title":"A","body":"message survives JSON release","from":"Muse","time":"today"}],"latest_id":1,"podcast_latest_id":4})";
std::string HttpGet(const std::string&,int* status) { *status=200; return body; }
std::string JsonString(cJSON* object,const char* key) {
 auto* item=cJSON_GetObjectItem(object,key);
 return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
void SaveLater(const char*,const std::string&) {}
namespace tab5_muse {
''' + structs + r'''
class Inbox {
public:
 std::mutex mutex_;
 Snapshot snapshot_;
 std::string topic_;
 int failures_=0;
 std::function<void(const Snapshot&)> listener_;
 bool fetch_ok=true;
 tab5_podcast::EpisodeList feed;
 bool FetchPodcasts(const std::string&,tab5_podcast::EpisodeList* out) {
   // TLS and episode parsing must never overlap with the inbox's JSON tree.
   assert(json_allocations==0);
   if (!fetch_ok) return false;
   *out=feed;return true;
 }
 bool Discover() {return false;}
 bool Poll();
};
''' + poll + r'''
}
int main() {
 cJSON_Hooks hooks{JsonAlloc,JsonFree};cJSON_InitHooks(&hooks);
 tab5_muse::Inbox inbox;
 inbox.snapshot_.host="localhost";
 tab5_podcast::Episode episode;episode.id=4;episode.title="Daily radio";
 episode.tracks={{"Song","Artist"}};inbox.feed={episode};
 int callbacks=0;tab5_muse::Snapshot seen;
 inbox.listener_=[&](const auto& value){++callbacks;seen=value;};
 assert(inbox.Poll() && callbacks==1 && seen.podcast_ok && seen.poll_count==1);
 assert(json_allocations==0 && seen.messages.size()==1);
 assert(seen.messages[0].body=="message survives JSON release");
 auto first=seen.episodes;
 // All inbox fields and episode IDs stay identical. Only late MP3 delivery changes.
 inbox.feed[0].audio_url="https://relay.example/narration.mp3";
 assert(inbox.Poll() && callbacks==2 && seen.poll_count==2);
 assert(seen.episodes!=first && (*seen.episodes)[0].audio_url==inbox.feed[0].audio_url);
 assert((*first)[0].audio_url.empty());
 auto updated=seen.episodes;
 assert(inbox.Poll() && callbacks==3 && seen.poll_count==3 && seen.episodes==updated);
 // A failing episode request must finish the refresh with an error while preserving data.
 inbox.fetch_ok=false;
 assert(inbox.Poll() && callbacks==4 && !seen.podcast_ok && seen.episodes==updated);
 body="invalid";
 assert(!inbox.Poll() && callbacks==5 && !seen.podcast_ok && seen.episodes==updated);
 assert(seen.poll_count==5);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            cpp, obj, executable = temp/'poll.cc', temp/'cjson.o', temp/'poll'
            cpp.write_text(harness)
            subprocess.run(['cc','-c',str(CJSON/'cJSON.c'),'-o',str(obj)],check=True,capture_output=True,text=True)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(BOARD),
                            '-I',str(CJSON),str(cpp),str(obj),'-o',str(executable)],
                           check=True,capture_output=True,text=True)
            subprocess.run([str(executable)],check=True,capture_output=True,text=True,timeout=10)


if __name__ == '__main__':
    unittest.main()
