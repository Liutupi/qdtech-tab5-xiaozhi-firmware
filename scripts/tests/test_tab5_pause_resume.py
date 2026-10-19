"""Exercise the real player controls: Pause preserves identity; Stop invalidates it."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD=Path(__file__).resolve().parents[2]/'main/boards/qdtech/tab5'

@unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
class PauseResumeTests(unittest.TestCase):
    def test_active_pause_resume_stop_and_pending_replacement(self):
        source=(BOARD/'radio_service.cc').read_text()
        methods=source[source.index('void RadioService::PlayPause()'):source.index('void RadioService::SetPlaybackReleasedCallback(')]
        harness=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "tab5_audio_stream.h"
#define ESP_LOGI(...) ((void)0)
class RadioService {
public:
 enum class Command {PLAY_PAUSE,PAUSE,STOP,FOCUS_CHANGED};
 std::mutex submission_mutex_, pending_mutex_;
 std::atomic<bool> play_requested_{true},stop_requested_{false},replacement_pending_{false},user_paused_{false};
 std::atomic<bool> playback_release_pending_{false};
 std::atomic<uint32_t> stream_generation_{7}, command_sequence_{1};
 std::atomic<int> music_playback_state_{1};
 bool pending_custom_valid_=false,pending_toggle_parity_=false;
 int pending_station_index_=-1,pending_navigation_steps_=0;
 std::vector<Command> commands;
 std::string state;
 void PostCommand(Command c) {commands.push_back(c);}
 void SetUi(const char* s,const char*) {state=s;}
 void PlayPause();void Play();void Pause();void Stop();
};
'''+methods+r'''
int main() {
 RadioService radio;
 tab5_audio_stream::ResumePoint bookmark;bookmark.Capture("original",7,10000,800);
 radio.PlayPause();assert(radio.user_paused_ && radio.play_requested_ && !radio.stop_requested_);
 assert(radio.stream_generation_==7 && radio.commands.empty() && radio.state=="Paused");
 radio.PlayPause();assert(!radio.user_paused_ && radio.state=="Connecting");
 const auto before=radio.command_sequence_.load();
 radio.pending_navigation_steps_=3;
 radio.Pause();radio.Pause();assert(radio.user_paused_ && radio.commands.empty());
 assert(radio.pending_navigation_steps_==0 && radio.command_sequence_==before+2);
 radio.Play();assert(!radio.user_paused_ && radio.stream_generation_==7 && radio.commands.empty());
 for(int i=0;i<1000;++i)radio.PlayPause();
 assert(!radio.user_paused_ && bookmark.Matches("original",radio.stream_generation_));
 radio.Pause();radio.Stop();assert(!radio.user_paused_ && !radio.play_requested_ && radio.stop_requested_);
 assert(radio.commands.back()==RadioService::Command::STOP && radio.stream_generation_==8);
 assert(!bookmark.Matches("original",radio.stream_generation_));
 radio.commands.clear();radio.PlayPause();
 assert(radio.commands.back()==RadioService::Command::PLAY_PAUSE && radio.stream_generation_==9);
 // A new source waiting to be accepted must be cancelled, not resumed as the old song.
 radio.replacement_pending_=true;radio.pending_custom_valid_=true;
 radio.Pause();assert(!radio.replacement_pending_ && !radio.pending_custom_valid_);
 assert(radio.commands.back()==RadioService::Command::PAUSE && radio.stream_generation_==10);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp,exe=Path(directory)/'pause.cc',Path(directory)/'pause'
            cpp.write_text(harness)
            result=subprocess.run(['c++','-std=c++17','-I',str(BOARD),str(cpp),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
