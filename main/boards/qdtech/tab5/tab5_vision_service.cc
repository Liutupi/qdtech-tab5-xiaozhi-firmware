#include "tab5_vision_service.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>


#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "esp_video.h"
#include "nabo_assets.h"
#include "pedestrian_detect.hpp"
#include "tab5_native_display.h"

namespace {
constexpr char kTag[] = "Tab5Vision";
constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kGridWidth = 32;
constexpr int kGridHeight = 24;
constexpr int64_t kSleepAfterUs = 90LL * 1000000;
constexpr int64_t kGreetingCooldownUs = 120LL * 1000000;
constexpr int64_t kGreetingRequestLifetimeUs = 8LL * 1000000;
}

void Tab5VisionService::Start(EspVideo* camera, QdtechTab5Display* display) {
    if (started_ || !camera || !display) return;
    camera_ = camera;
    display_ = display;
    // A measured person-detection run used about 4.4 KB of stack. Retain
    // more than 7 KB of headroom while returning 12 KB of internal RAM.
    started_ = xTaskCreate(TaskEntry, "nabo_vision", 12288, this, 3, nullptr) == pdPASS;
    if (!started_)
        ESP_LOGE(kTag, "Failed to start camera vision task");
}

void Tab5VisionService::RequestPresenceTest() {
    test_requested_.store(true);
    if (!display_) return;
    if (vision_ready_.load()) {
        display_->SetVisionMessage("感应测试", "左侧显示摄像头画面；让人进入取景。");
    } else {
        display_->SetVisionMessage("准备感应", "摄像头启动后会自动测试，请稍候。");
    }
}

void Tab5VisionService::TaskEntry(void* arg) {
    static_cast<Tab5VisionService*>(arg)->Run();
    vTaskDelete(nullptr);
}

void Tab5VisionService::TryGreeting(int64_t detected_at_us) {
    if (!greeting_owed_.load(std::memory_order_acquire) ||
        greeting_queued_.exchange(true, std::memory_order_acq_rel))
        return;
    Application::GetInstance().Schedule([this, detected_at_us] {
        auto& app = Application::GetInstance();
        const int64_t now = esp_timer_get_time();
        if (greeting_owed_.load(std::memory_order_acquire) &&
            now - detected_at_us < kGreetingRequestLifetimeUs && !app.IsExternalAudioActive() &&
            app.GetDeviceState() == kDeviceStateIdle) {
            display_->WelcomeBack();
            app.PlaySound(std::string_view(reinterpret_cast<const char*>(nabo_greeting_ogg),
                                           nabo_greeting_ogg_len));
            last_greeting_us_.store(now, std::memory_order_release);
            greeting_owed_.store(false, std::memory_order_release);
        }
        // When an audio conversation wins the race, retain the greeting intent.
        // The next confirmed person frame retries after audio becomes idle.
        greeting_queued_.store(false, std::memory_order_release);
    });
}

void Tab5VisionService::Run() {
    constexpr size_t kRgbBytes = size_t(kWidth) * kHeight * 3;
    auto* rgb =
        static_cast<uint8_t*>(heap_caps_malloc(kRgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int64_t next_rgb_attempt = 0;
    if (!rgb)
        ESP_LOGW(kTag, "No PSRAM for vision frame; will retry while UI stays responsive");
    std::array<uint8_t, kGridWidth * kGridHeight> previous{};
    bool have_previous = false;
    bool sleeping = false;
    bool camera_paused = false;
    bool person_present = false;
    bool test_pending = false;
    bool test_deferred_for_audio = false;
    bool test_saw_person = false;
    unsigned motion_history = 0;
    unsigned person_frames = 0;
    unsigned failed_frames = 0;
    unsigned sample_count = 0;
    unsigned detection_count = 0;
    int gain_index = 96;
    int64_t last_activity = esp_timer_get_time();
    int64_t last_confirmed_motion = 0;
    int64_t last_person_seen = 0;
    int64_t last_gain_adjust = 0;
    int64_t last_person_scan = 0;
    int64_t test_wait_start = 0;
    int64_t test_deadline = 0;
    const int64_t started_at = esp_timer_get_time();
    std::unique_ptr<PedestrianDetect> detector;
    int64_t next_detector_attempt = started_at + 25LL * 1000000;

    // The camera, SD host and audio network startup share resources at boot.
    // Sleep in slices so radio/music can stop the CSI stream immediately.
    {
        const int64_t wait_until = esp_timer_get_time() + 7LL * 1000 * 1000;
        while (esp_timer_get_time() < wait_until) {
            if (Application::GetInstance().IsExternalAudioActive()) {
                if (!camera_paused)
                    camera_paused = camera_->PauseStream();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    bool manual_gain = camera_->ConfigureVisionLowLight();
    if (!manual_gain)
        ESP_LOGW(kTag, "Camera manual exposure or gain unavailable");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time();
        if (interaction_requested_.exchange(false)) {
            last_activity = now;
            sleeping = false;
            display_->SetSleeping(false);
        }
        const bool new_test = test_requested_.exchange(false);
        if (new_test) {
            test_pending = true;
            test_wait_start = now;
            test_deadline = 0;
            test_saw_person = false;
            last_activity = now;
            sleeping = false;
            display_->SetSleeping(false);
        }
        const bool chatting = Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
        const bool radio_active = Application::GetInstance().IsExternalAudioActive();
        if (chatting || radio_active)
            last_activity = now;
        if ((chatting || radio_active) && (test_pending || test_deadline) &&
            (!test_deferred_for_audio || new_test)) {
            test_pending = true;
            test_wait_start = now;
            test_deadline = 0;
            test_saw_person = false;
            test_deferred_for_audio = true;
            display_->SetVisionPreview(nullptr);
            display_->SetVisionMessage(
                radio_active ? "播放中暂停感应" : "对话中暂停感应",
                radio_active ? "停止播放后将自动开始感应测试。" : "对话结束后将自动开始感应测试。");
        }
        // Camera CSI + motion grid compete with radio/music/NES on a tight
        // DMA and power budget. Fully stop the stream while external audio
        // (radio, music, or the emulator) runs.
        if (radio_active) {
            have_previous = false;
            if (!camera_paused) {
                camera_paused = camera_->PauseStream();
                if (camera_paused) {
                    ESP_LOGI(kTag, "Camera off while radio/music plays");
                }
            }
            continue;
        }
        if (chatting) {
            have_previous = false;
            continue;
        }
        if (test_deferred_for_audio) {
            test_deferred_for_audio = false;
            test_wait_start = now;
        }
        if (test_pending && now - test_wait_start > 60LL * 1000000) {
            test_pending = false;
            display_->SetVisionPreview(nullptr);
            display_->SetVisionMessage("感应暂不可用", "摄像头未准备好，请稍后再试。");
        }
        if (test_deadline && now > test_deadline) {
            test_deadline = 0;
            display_->SetVisionPreview(nullptr);
            display_->SetVisionMessage(test_saw_person ? "检测到有人" : "暂未检测到人",
                                       test_saw_person ? "有人靠近时，Nabo 会主动问候。"
                                                       : "请让人出现在画面中，再试一次。");
        }
        if (camera_paused) {
            if (!camera_->ResumeStream()) {
                ESP_LOGW(kTag, "Camera resume failed; will retry");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            camera_paused = false;
            manual_gain = camera_->ConfigureVisionLowLight();
            if (manual_gain && camera_->SetVisionGainIndex(gain_index)) {
                // keep the last indoor gain after re-enabling the sensor
            }
            have_previous = false;
            ESP_LOGI(kTag, "Camera on after radio/music stopped");
        }
        if (!rgb) {
            if (now < next_rgb_attempt)
                continue;
            rgb = static_cast<uint8_t*>(
                heap_caps_malloc(kRgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!rgb) {
                next_rgb_attempt = now + 10LL * 1000000;
                ESP_LOGW(kTag, "Vision frame allocation still unavailable");
                continue;
            }
            ESP_LOGI(kTag, "Vision frame allocation recovered");
        }
        uint16_t width = 0, height = 0;
        if (!camera_->CaptureVisionFrame(rgb, kRgbBytes, width, height) || width != kWidth ||
            height != kHeight) {
            if (++failed_frames % 30 == 0)
                ESP_LOGW(kTag, "Camera sample unavailable (%u)", failed_frames);
            continue;
        }
        if (failed_frames) {
            ESP_LOGI(kTag, "Camera sampling recovered after %u misses", failed_frames);
            failed_frames = 0;
        }
        // AFE buffers settle asynchronously; defer the model until voice is idle.
        if (!detector && now >= next_detector_attempt &&
            !Application::GetInstance().IsExternalAudioActive() &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            next_detector_attempt = now + 10LL * 1000000;
            const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            const size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
            // The vendor constructor assumes model allocation succeeds. The packed model is
            // about 425 KB and its inference arena is 539 KB. Leave room for concurrent
            // UI/audio after the LZ4 pose assets have been mapped into PSRAM.
            if (free_psram >= 1792 * 1024 && largest_psram >= 1280 * 1024) {
                detector = std::make_unique<PedestrianDetect>(PedestrianDetect::PICO_S8_V1, false);
                vision_ready_.store(true);
                ESP_LOGI(kTag, "Person detector ready; PSRAM before=%u after=%u largest=%u",
                         unsigned(free_psram),
                         unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                         unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
            } else {
                ESP_LOGW(kTag, "Deferring person detector: PSRAM free=%u largest=%u",
                         unsigned(free_psram), unsigned(largest_psram));
            }
        }

        std::array<uint8_t, kGridWidth * kGridHeight> current{};
        int sum_delta = 0;
        for (int y = 0; y < kGridHeight; ++y) {
            for (int x = 0; x < kGridWidth; ++x) {
                const int sx = x * kWidth / kGridWidth;
                const int sy = y * kHeight / kGridHeight;
                int luma = 0;
                for (int dy : {1, 6}) {
                    for (int dx : {2, 7}) {
                        const uint8_t* p = rgb + (size_t(sy + dy) * kWidth + sx + dx) * 3;
                        luma += (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8;
                    }
                }
                luma /= 4;
                current[y * kGridWidth + x] = luma;
                sum_delta += luma - previous[y * kGridWidth + x];
            }
        }
        if (manual_gain && now - last_gain_adjust >= 3LL * 1000000) {
            auto levels = current;
            auto percentile = levels.begin() + levels.size() * 3 / 4;
            std::nth_element(levels.begin(), percentile, levels.end());
            const int brightness = *percentile;
            int next = gain_index;
            if (brightness < 70) next = std::min(gain_index + 16, 128);
            else if (brightness > 145) next = std::max(gain_index - 16, 0);
            if (next != gain_index && camera_->SetVisionGainIndex(next)) {
                gain_index = next;
                ESP_LOGI(kTag, "Camera gain index %d; brightness p75=%d", gain_index, brightness);
            }
            last_gain_adjust = now;
        }

        int changed = 0;
        if (have_previous) {
            const int global_shift = sum_delta / int(current.size());
            for (size_t i = 0; i < current.size(); ++i) {
                if (std::abs(int(current[i]) - previous[i] - global_shift) > 20) ++changed;
            }
        }
        previous = current;
        have_previous = true;
        motion_history = ((motion_history << 1) | unsigned(changed > 42)) & 15;
        const bool motion = changed > 100 || __builtin_popcount(motion_history) >= 2;
        if (motion) {
            last_activity = now;
            last_confirmed_motion = now;
            if (sleeping) {
                sleeping = false;
                display_->SetSleeping(false);
                ESP_LOGI(kTag, "Motion woke Nabo: %d/768 cells", changed);
            }
        }
        if (++sample_count % 10 == 0)
            ESP_LOGI(kTag, "Camera active: scene change %d/768, asleep=%d", changed, sleeping);
        if (sample_count % 60 == 0)
            ESP_LOGI(kTag,
                     "Vision memory: stack_min_free=%u internal_free=%u largest_internal=%u "
                     "psram_free=%u",
                     unsigned(uxTaskGetStackHighWaterMark(nullptr)),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

        if (test_pending && detector) {
            test_pending = false;
            test_deadline = now + 20LL * 1000000;
            display_->SetVisionMessage("感应测试", "让人进入左侧取景中央。");
            ESP_LOGI(kTag, "Person presence test started");
        }
        if (test_pending || test_deadline) {
            last_activity = now;
            display_->SetVisionPreview(rgb);
        }

        if (!chatting && !sleeping && now - last_activity > kSleepAfterUs) {
            sleeping = true;
            display_->SetSleeping(true);
            ESP_LOGI(kTag, "No activity for 90s; Nabo sleeping");
        }
        if (!detector || Application::GetInstance().IsExternalAudioActive() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            continue;
        const int64_t scan_interval =
            test_deadline || (last_confirmed_motion && now - last_confirmed_motion < 10LL * 1000000) ||
            (person_frames && !person_present) ? 1000000 :
            sleeping ? 5000000 : person_present ? 3000000 : 2000000;
        if (now - last_person_scan < scan_interval) continue;
        last_person_scan = now;
        dl::image::img_t image = {rgb, width, height, dl::image::DL_IMAGE_PIX_TYPE_RGB888};
        auto& people = detector->run(image);
        if (test_deadline) {
            test_saw_person |= !people.empty();
            display_->SetVisionMessage(people.empty() ? "正在寻找人" : "检测到有人",
                                       people.empty() ? "请让人进入取景中央。" :
                                       "保持现在的位置，Nabo 已经看到你了。");
        }
        if (++detection_count % 10 == 0 || test_deadline)
            ESP_LOGI(kTag, "Person detector: %u match(es)", unsigned(people.size()));

        if (!people.empty()) {
            last_person_seen = now;
            person_frames = std::min(person_frames + 1, 3U);
            const bool strong_match = std::any_of(people.begin(), people.end(),
                [](const auto& person) { return person.score >= 0.90f; });
            if ((person_frames >= 2 || strong_match) && !person_present) {
                person_present = true;
                last_activity = now;
                if (sleeping) {
                    sleeping = false;
                    display_->SetSleeping(false);
                }
                ESP_LOGI(kTag, "Person arrived");
                const int64_t last_greeting = last_greeting_us_.load(std::memory_order_acquire);
                greeting_owed_.store(!last_greeting || now - last_greeting >= kGreetingCooldownUs,
                                     std::memory_order_release);
            }
            if (person_present)
                TryGreeting(now);
        } else {
            person_frames = 0;
            if (person_present && now - last_person_seen > 15LL * 1000000) {
                person_present = false;
                greeting_owed_.store(false, std::memory_order_release);
                ESP_LOGI(kTag, "Person left the camera view");
            }
        }
    }
}
