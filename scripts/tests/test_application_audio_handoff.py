"""Run actual application callbacks against a controllable network channel."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def application_harness(source):
    def section(start, end):
        return source[source.index(start):source.index(end, source.index(start))]

    channel = section('void Application::ContinueOpenAudioChannel(',
                      'void Application::HandleStartListeningEvent(')
    listening = section('void Application::StartListeningAudio()',
                        'void Application::ConfigureWakeWordForListening()')
    tts = source[source.index('} else if (strcmp(type->valuestring, "tts") == 0)'):]
    start = tts.index('Schedule([this]() {')
    first = tts[start:tts.index('});', start) + 3]
    stop = tts.index('} else if (strcmp(state->valuestring, "stop") == 0)')
    start = tts.index('Schedule([this]() {', stop)
    second = tts[start:tts.index('});', start) + 3]
    return r'''
#include <functional>
#include <memory>
#include <string_view>
#include <vector>
enum DeviceState {kDeviceStateIdle,kDeviceStateConnecting,kDeviceStateListening,kDeviceStateSpeaking};
enum ListeningMode {kListeningModeAutoStop,kListeningModeManualStop};
enum class PowerSaveLevel {PERFORMANCE};
struct Camera {void PauseStream(){}};
struct Board {
    static Board& GetInstance(){static Board b;return b;}
    void SetPowerSaveLevel(PowerSaveLevel){}
    void PrepareForNetwork(){}
    Camera* GetCamera(){return nullptr;}
};
struct Background {
    std::vector<std::function<void()>> jobs;
    void Schedule(std::function<void()> f){jobs.push_back(f);}
    void Run(){auto f=jobs.front();jobs.erase(jobs.begin());f();}
};
struct Protocol {
    bool opened=false;int closes=0,starts=0;
    std::function<void()> on_open;
    bool IsAudioChannelOpened(){return opened;}
    bool OpenAudioChannel(){opened=true;if(on_open)on_open();return true;}
    void CloseAudioChannel(){++closes;opened=false;}
    void SendStartListening(ListeningMode){++starts;}
};
struct Audio {
    int enables=0,popups=0;
    void EnableVoiceProcessing(bool b){if(b)++enables;}
    void PlaySound(std::string_view){++popups;}
};
namespace Lang {namespace Sounds {constexpr std::string_view OGG_POPUP="popup";}}
class Application {
public:
    DeviceState state=kDeviceStateConnecting;
    bool external=false,pending_listening_start_=false,play_popup_on_listening_=false,aborted_=true;
    ListeningMode listening_mode_=kListeningModeAutoStop;
    int listening_changes=0;
    Protocol channel;Protocol* protocol_=&channel;
    Audio audio_service_;Background background;
    DeviceState GetDeviceState(){return state;}
    bool IsExternalAudioActive(){return external;}
    void SetDeviceState(DeviceState s){state=s;}
    void SetListeningMode(ListeningMode m){listening_mode_=m;state=kDeviceStateListening;++listening_changes;}
    Background* GetBackgroundTask(){return &background;}
    void Schedule(std::function<void()> f){background.Schedule(f);}
    void ConfigureWakeWordForListening(){}
    void ContinueOpenAudioChannel(ListeningMode);
    void StartListeningAudio();
    void QueueTtsStart();void QueueTtsStop();
};
''' + channel + listening + '\nvoid Application::QueueTtsStart(){' + first + '}' + \
        '\nvoid Application::QueueTtsStop(){' + second + '}' + r'''
int main() {
    Application normal;
    normal.ContinueOpenAudioChannel(kListeningModeAutoStop);normal.background.Run();
    if(normal.state!=kDeviceStateListening || normal.listening_changes!=1) return 1;
    Application canceled;
    canceled.channel.on_open=[&]{canceled.state=kDeviceStateIdle;canceled.external=true;};
    canceled.ContinueOpenAudioChannel(kListeningModeAutoStop);canceled.background.Run();
    if(canceled.state!=kDeviceStateIdle || canceled.listening_changes || canceled.channel.closes!=1) return 2;
    Application manual;
    manual.external=true;manual.state=kDeviceStateListening;
    manual.StartListeningAudio();
    if(!manual.pending_listening_start_ || manual.channel.starts || manual.audio_service_.enables) return 3;
    manual.external=false;manual.StartListeningAudio();
    if(manual.channel.starts!=1 || manual.audio_service_.enables!=1) return 4;
    Application late;
    late.state=kDeviceStateIdle;late.QueueTtsStart();late.external=true;late.background.Run();
    if(late.state!=kDeviceStateIdle || !late.aborted_) return 5;
    late.state=kDeviceStateSpeaking;late.QueueTtsStop();late.background.Run();
    if(late.state!=kDeviceStateSpeaking) return 6;
    late.external=false;late.QueueTtsStart();late.background.Run();
    if(late.state!=kDeviceStateSpeaking || late.aborted_) return 7;
    late.QueueTtsStop();late.background.Run();
    if(late.state!=kDeviceStateListening) return 8;
}
'''


class ApplicationHandoffTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_late_network_and_tts_callbacks_cannot_restart_music_microphone(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/application.cc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'application.cc'
            binary = Path(directory) / 'application'
            test.write_text(application_harness(source))
            subprocess.run(['c++', '-std=c++17', '-pthread', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
