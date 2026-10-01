"""Compile real audio entry points with deterministic host peripherals."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def method(source, signature, next_signature):
    return source[source.index(signature):source.index(next_signature, source.index(signature))]


def audio_harness(source):
    functions = ''.join([
        method(source, 'void AudioService::EnableWakeWordDetection(',
               'void AudioService::ReleaseWakeWordResources('),
        method(source, 'void AudioService::EnableVoiceProcessing(',
               'void AudioService::EnableAudioTesting('),
        method(source, 'void AudioService::SetExternalPlaybackActive(',
               'void AudioService::NoteOutputActivity('),
        method(source, 'void AudioService::PlaySound(', 'bool AudioService::IsIdle('),
        method(source, 'bool AudioService::PushPacketToDecodeQueue(',
               'std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue('),
    ])
    return r'''
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
#define CONFIG_IDF_TARGET_ESP32P4 1
#define ESP_LOGD(...) ((void)0)
constexpr unsigned AS_EVENT_WAKE_WORD_RUNNING=1, AS_EVENT_AUDIO_PROCESSOR_RUNNING=2;
constexpr int AUDIO_POWER_CHECK_INTERVAL_MS=1, MAX_DECODE_PACKETS_IN_QUEUE=10;
void xEventGroupClearBits(unsigned* g,unsigned b){*g &= ~b;}
void xEventGroupSetBits(unsigned* g,unsigned b){*g |= b;}
void esp_timer_stop(void*){}
void esp_timer_start_periodic(void*,int){}
void esp_ae_rate_cvt_reset(void*){}
struct Engine {
    bool wake=false,voice=false;
    bool HasWakeWord(){return true;}
    void EnableWakeWordDetection(bool b){wake=b;}
    void EnableVoiceProcessing(bool b){voice=b;}
};
struct Codec { bool output_enabled(){return true;} void EnableOutput(bool){} };
struct AudioStreamPacket {
    int sample_rate=0,frame_duration=0;std::vector<uint8_t> payload;
};
class OggDemuxer {
public:
    std::function<void(const uint8_t*,int,int,size_t)> cb;
    void OnPacket(decltype(cb) f){cb=f;}
    void Reset(){}
    void Process(const uint8_t* b,size_t n){cb(b,16000,60,n);}
};
class AudioService {
public:
    std::mutex audio_control_mutex_,input_resampler_mutex_,audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::atomic<bool> external_playback_active_{false},service_stopped_{false};
    bool audio_engine_initialized_=true,audio_input_need_warmup_=false;
    Engine engine; Engine* audio_engine_=&engine;
    Codec codec; Codec* codec_=&codec;
    unsigned bits=0;unsigned* event_group_=&bits;
    void* input_resampler_=nullptr;void* audio_power_timer_=nullptr;
    unsigned playback_generation_=0;bool playback_drained_notified_=true;
    std::vector<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    bool InitializeAudioEngine(){return true;}
    void ResetDecoder(){}
    void NoteOutputActivity(){}
    void EnableWakeWordDetection(bool);
    void EnableVoiceProcessing(bool);
    void SetExternalPlaybackActive(bool);
    void PlaySound(const std::string_view&);
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>,bool);
};
''' + functions + r'''
int main() {
    AudioService audio;
    audio.EnableWakeWordDetection(true);audio.EnableVoiceProcessing(true);
    if(!audio.engine.wake || !audio.engine.voice) return 1;
    audio.SetExternalPlaybackActive(true);
    audio.EnableWakeWordDetection(true);audio.EnableVoiceProcessing(true);
    if(audio.engine.wake || audio.engine.voice || audio.bits) return 2;
    audio.PlaySound("greeting");
    if(!audio.audio_decode_queue_.empty()) return 3;
    if(audio.PushPacketToDecodeQueue(std::make_unique<AudioStreamPacket>(),false)) return 4;
    audio.SetExternalPlaybackActive(false);
    audio.EnableWakeWordDetection(true);audio.EnableVoiceProcessing(true);
    audio.PlaySound("normal prompt");
    if(!audio.engine.wake || !audio.engine.voice || audio.audio_decode_queue_.size()!=1) return 5;
    for(int i=0;i<100;++i) {
        AudioService race;
        std::thread mic([&]{race.EnableVoiceProcessing(true);race.EnableWakeWordDetection(true);});
        std::thread owner([&]{race.SetExternalPlaybackActive(true);});
        mic.join();owner.join();
        if(race.engine.wake || race.engine.voice || race.bits) return 6;
    }
}
'''


class ExternalAudioTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_music_owns_microphone_and_prompt_output_until_release(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / 'main/audio/audio_service.cc').read_text()
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'audio.cc'
            binary = Path(directory) / 'audio'
            test.write_text(audio_harness(source))
            subprocess.run(['c++', '-std=c++17', '-pthread', str(test), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
