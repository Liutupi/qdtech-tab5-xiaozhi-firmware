"""Execute the service's real start/stop methods through the restart handoff."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/fc_emulator_service.cc"

def function(source, name):
    start = source.index("void FcEmulatorService::" + name + "(")
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

class NesRestartTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_audio_queue_is_bounded_and_rejects_previous_game(self):
        source = SOURCE.read_text()
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <cstdint>
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
struct AudioFrame {uint32_t generation; int count,rate; int16_t samples[512];};
std::deque<AudioFrame> queue;
int xQueueSend(void*,const AudioFrame* f,unsigned wait) {
 assert(wait==0); if(queue.size()==3)return 0;queue.push_back(*f);return 1;
}
struct Done {};
int xQueueReceive(void*,AudioFrame* f,unsigned wait) {
 if(queue.empty()){if(wait==portMAX_DELAY)throw Done{};return 0;}
 *f=queue.front();queue.pop_front();return 1;
}
void xSemaphoreTake(void*,unsigned){}
void xSemaphoreGive(void*){}
class FcEmulatorService {
public:
 using AudioFrame=::AudioFrame;
 void* audio_queue_=this;void* audio_mutex_=this;
 std::atomic<bool> playing_{true};std::atomic<uint32_t> audio_generation_{2};
 int writes=0;int16_t received=0;
 void QueueNofrendoAudio(const int16_t*,int,int);
 static void AudioTaskWrapper(void*);
 void WriteNofrendoAudio(const int16_t* s,int count,int rate) {
  assert(count==400 && rate==24000);++writes;received=s[0];
 }
};
''' + function(source, "QueueNofrendoAudio") + "\n" + function(source, "AudioTaskWrapper") + r'''
int main() {
 FcEmulatorService f;int16_t samples[513]={};
 for(int n=1;n<=4;++n){samples[0]=n;f.QueueNofrendoAudio(samples,400,24000);}
 assert(queue.size()==3 && queue.front().samples[0]==2 && queue.back().samples[0]==4);
 f.QueueNofrendoAudio(samples,513,24000);assert(queue.size()==3);
 f.QueueNofrendoAudio(nullptr,400,24000);assert(queue.size()==3);
 queue.front().generation=1; // A packet already received before a quick restart.
 try{FcEmulatorService::AudioTaskWrapper(&f);}catch(Done&){}
 assert(f.writes==2 && f.received==4);
 f.QueueNofrendoAudio(samples,400,24000);f.playing_.store(false);
 try{FcEmulatorService::AudioTaskWrapper(&f);}catch(Done&){}
 assert(f.writes==2 && queue.empty());
 f.QueueNofrendoAudio(samples,400,24000);assert(queue.empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "audio_queue.cc"
            cpp.write_text(harness)
            binary = Path(directory) / "audio_queue"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_quick_restart_and_plain_stop(self):
        source = SOURCE.read_text()
        harness = r'''
#include <atomic>
#include <cassert>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
int stops=0;
void qd_nofrendo_request_stop() {++stops;}
class FcEmulatorService {
public:
 std::atomic<bool> start_requested_{false},play_after_stop_{false},playing_{false},active_{true};
 std::atomic<bool> scan_requested_{false},play_after_scan_{false};
 std::atomic<int> controller_state_{0},controller_release_tick_{0};
 std::vector<int> roms_{1};
 void SetActive(bool active){active_.store(active);}
 void PublishMode(bool){}
 void PublishState(const char*,const char*){}
 std::string SelectedName(){return "test.nes";}
 void Stop(bool restart=false);
 void StartSelected();
};
''' + function(source,"Stop") + "\n" + function(source,"StartSelected") + r'''
int main() {
 FcEmulatorService f;
 f.playing_.store(true);
 f.StartSelected();
 assert(stops==1 && f.play_after_stop_.load());
 // The emulator owns final teardown and consumes this flag afterwards.
 f.playing_.store(false);
 if(f.play_after_stop_.exchange(false) && f.active_.load())f.start_requested_.store(true);
 assert(f.start_requested_.load());
 f.playing_.store(true);f.Stop();
 assert(stops==2 && !f.play_after_stop_.load() && !f.start_requested_.load());
 f.playing_.store(false);f.StartSelected();
 assert(f.start_requested_.load());
 f.Stop();assert(!f.start_requested_.load());
 f.Stop(true);assert(f.start_requested_.load());
 f.Stop();assert(!f.start_requested_.load());
 f.roms_.clear();f.StartSelected();
 assert(f.scan_requested_.load() && f.play_after_scan_.load());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp=Path(directory)/"restart.cc";cpp.write_text(harness)
            binary=Path(directory)/"restart"
            subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",
                            "-fsanitize=address,undefined",str(cpp),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__ == "__main__":
    unittest.main()
