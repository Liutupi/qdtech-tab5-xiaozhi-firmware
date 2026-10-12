"""Exercise the real MultiNet feed/lifecycle code with a deterministic decoder."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <esp_heap_caps.h>
#define private public
#include "custom_wake_word.h"
#undef private
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int64_t esp_timer_get_time() { return 0; }
static unsigned uxTaskGetStackHighWaterMark(void*) { return 1024; }

CustomWakeWord::CustomWakeWord() = default;
CustomWakeWord::~CustomWakeWord() = default;

static esp_mn_state_t detect(model_iface_data_t* model, const int16_t* pcm) {
    ++model->calls;
    ++model->frames;
    model->max_frames = std::max(model->max_frames, model->frames);
    for (int i = 0; i < 512; ++i) {
        const int value = pcm[i];
        model->fed.push_back(value);
        if (value == 301) model->pattern = 1;
        if (value == 302 && model->pattern == 1) model->pattern = 2;
        if (value == 303 && model->pattern == 2) return ESP_MN_STATE_DETECTED;
    }
    return model->next_state;
}
static void clean(model_iface_data_t* model) {
    ++model->cleans;
    model->frames = 0;
    model->pattern = 0;
}
static esp_mn_results_t* results(model_iface_data_t* model) {
    return model->null_result ? nullptr : &model->result;
}
static int chunk_size(model_iface_data_t*) { return 512; }

__METHODS__

struct Fixture {
    model_iface_data_t model;
    esp_mn_iface_t api{detect, clean, results, chunk_size};
    AudioCodec codec;
    CustomWakeWord word;
    int wakes = 0;
    Fixture() {
        word.multinet_ = &api;
        word.multinet_model_data_ = &model;
        word.codec_ = &codec;
        word.chunk_samples_ = 512;
        word.input_buffer_.reserve(512);
        word.history_chunk_.resize(512);
        assert(word.recognition_history_.Initialize(32000));
        assert(word.wake_word_audio_cache_.Initialize(32000));
        word.commands_.push_back({"ni hao na bo", "Nabo", "wake"});
        word.OnWakeWordDetected([this](const std::string& value) {
            assert(value == "Nabo");
            ++wakes;
            // Callback re-entry must not deadlock on the decoder lock.
            word.Stop();
        });
        word.Start();
    }
    void feed(size_t count, bool speech, int16_t value = 0) {
        std::vector<int16_t> pcm(count, value);
        word.FeedMono(pcm.data(), pcm.size(), speech);
    }
};

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string scenario = argv[1];
    Fixture f;
    if (scenario == "idle") {
        for (int i = 0; i < 3600; ++i) f.feed(16000, false);
        assert(f.model.calls == 0);
        assert(f.word.recognition_history_.Size() == 32000);
        assert(f.word.input_buffer_.capacity() == 512);
    } else if (scenario == "speech") {
        f.feed(16000, false, 7);
        f.feed(512, true, 8);
        assert(f.model.fed.front() == 7); // preserve VAD look-back
        assert(f.model.fed.size() <= 8512);
        f.feed(11999, false);
        assert(f.word.session_active_);
        f.feed(1, false);
        assert(!f.word.session_active_);
        const int calls = f.model.calls;
        f.feed(16000, false);
        assert(f.model.calls == calls);
    } else if (scenario == "continuous") {
        for (int i = 0; i < 120; ++i) f.feed(16000, true, 9);
        assert(f.model.max_frames <= 250); // never more than eight seconds
        assert(f.word.windows_total_ > 15);
        assert(f.word.input_buffer_.capacity() == 512);
    } else if (scenario == "boundary") {
        f.feed(126976, true); // 7.936s, just before the forced boundary
        f.feed(512, true, 301);
        f.feed(512, true, 302);
        assert(f.wakes == 0);
        f.feed(512, true, 303);
        assert(f.wakes == 1); // recognize a phrase straddling the reset
    } else if (scenario == "restart") {
        f.feed(17, true, 301);
        f.word.Stop();
        f.word.Start();
        f.model.fed.clear();
        f.feed(512, true, 5);
        assert(f.model.fed.size() == 512);
        assert(std::all_of(f.model.fed.begin(), f.model.fed.end(), [](int v) { return v == 5; }));
        assert(f.word.wake_word_audio_cache_.Size() == 512);
    } else if (scenario == "results") {
        f.model.next_state = ESP_MN_STATE_DETECTED;
        f.model.null_result = true;
        f.feed(512, true);
        f.model.null_result = false;
        f.model.result.num = 99;
        for (auto& id : f.model.result.command_id) id = 9999;
        f.feed(512, true);
        assert(f.wakes == 0);
        f.model.result.num = 1;
        f.model.result.command_id[0] = 1;
        f.feed(512, true);
        assert(f.wakes == 1);
        const int calls = f.model.calls;
        f.feed(512, true);
        assert(f.model.calls == calls);
    } else if (scenario == "timeout") {
        f.model.next_state = ESP_MN_STATE_TIMEOUT;
        f.feed(1024, true);
        assert(f.model.cleans == 4); // initial, speech start, two timeouts
        assert(f.wakes == 0);
    } else if (scenario == "stereo") {
        f.codec.channels = 2;
        f.word.Feed({1, 90, 2, 90, 3}); // partial interleaved input
        assert((f.word.input_buffer_ == std::vector<int16_t>{1, 2, 3}));
        f.word.FeedMono(nullptr, 512, true);
        assert(f.word.input_buffer_.size() == 3);
    } else {
        assert(false);
    }
}
'''


class CustomWakeWordSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('c++'):
            raise unittest.SkipTest('C++ compiler required')
        cls.directory = tempfile.TemporaryDirectory()
        directory = Path(cls.directory.name)
        headers = {
            'esp_attr.h': '',
            'freertos/FreeRTOS.h': '#include <cstdint>\nusing StackType_t = uint32_t;\n',
            'freertos/task.h': 'using TaskHandle_t = void*; struct StaticTask_t {};\n',
            'esp_mn_models.h': '#pragma once\n',
            'model_path.h': '#pragma once\nstruct srmodel_list_t {};\n',
            'audio_codec.h': '#pragma once\nstruct AudioCodec { int channels = 1; int input_channels() { return channels; } };\n',
            'wake_word.h': '#pragma once\nstruct WakeWord { virtual ~WakeWord() = default; };\n',
            'esp_log.h': '#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n',
            'esp_heap_caps.h': '''#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static void* heap_caps_malloc(size_t n, int) { return malloc(n); }
static void heap_caps_free(void* p) { free(p); }
static unsigned heap_caps_get_free_size(int) { return 50000; }
''',
            'esp_mn_iface.h': '''#pragma once
#include <cstdint>
#include <vector>
#define ESP_MN_RESULT_MAX_NUM 5
enum esp_mn_state_t { ESP_MN_STATE_DETECTING, ESP_MN_STATE_DETECTED, ESP_MN_STATE_TIMEOUT };
struct esp_mn_results_t { int num = 1; int command_id[5] = {1}; float prob[5] = {1}; };
struct model_iface_data_t {
    int calls = 0, cleans = 0, frames = 0, max_frames = 0, pattern = 0;
    bool null_result = false;
    esp_mn_state_t next_state = ESP_MN_STATE_DETECTING;
    esp_mn_results_t result;
    std::vector<int16_t> fed;
};
struct esp_mn_iface_t {
    esp_mn_state_t (*detect)(model_iface_data_t*, const int16_t*);
    void (*clean)(model_iface_data_t*);
    esp_mn_results_t* (*get_results)(model_iface_data_t*);
    int (*get_samp_chunksize)(model_iface_data_t*);
};
''',
        }
        for name, content in headers.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        source = (ROOT / 'main/audio/wake_words/custom_wake_word.cc').read_text()
        begin = source.index('void CustomWakeWord::OnWakeWordDetected(')
        methods = source[begin:source.index('size_t CustomWakeWord::GetFeedSize()', begin)]
        source_file = directory / 'sessions.cc'
        source_file.write_text(HARNESS.replace('__METHODS__', methods))
        cls.binary = directory / 'sessions'
        subprocess.run([
            'c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
            '-DCONFIG_SEND_WAKE_WORD_DATA=1', '-I', str(directory),
            '-I', str(ROOT / 'main/audio/wake_words'), str(source_file),
            str(ROOT / 'main/audio/wake_words/wake_word_audio_cache.cc'),
            '-o', str(cls.binary),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def check_scenario(self, scenario):
        subprocess.run([str(self.binary), scenario], check=True,
                       capture_output=True, text=True, timeout=30)

    def test_one_hour_silence_never_runs_decoder(self):
        self.check_scenario('idle')

    def test_vad_preroll_and_silence_tail(self):
        self.check_scenario('speech')

    def test_continuous_noise_has_bounded_windows(self):
        self.check_scenario('continuous')

    def test_wake_phrase_across_window_boundary(self):
        self.check_scenario('boundary')

    def test_restart_discards_old_audio(self):
        self.check_scenario('restart')

    def test_invalid_results_and_callback_reentry(self):
        self.check_scenario('results')

    def test_decoder_timeout_cleanup(self):
        self.check_scenario('timeout')

    def test_stereo_and_invalid_input(self):
        self.check_scenario('stereo')


if __name__ == '__main__':
    unittest.main()
