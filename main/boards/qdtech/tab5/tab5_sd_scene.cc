#include "tab5_sd_scene.h"
#ifdef CONFIG_QDTECH_TAB5_SD_SCENE
#include <cstdio>
#include <new>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nabo_sd_static.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "tab5_sd.h"

namespace {
constexpr size_t kBytes = 320 * 412 * 3;
constexpr const char* kPaths[] = {"/sdcard/tab5/nabo/idle.nab", "/sdcard/tab5/nabo/transition.nab",
                                  "/sdcard/tab5/nabo/sleep.nab", "/sdcard/tab5/nabo/work.nab"};
constexpr unsigned kCounts[] = {113, 81, 207, 50};
class SdSource : public nabo_sd::Source {
public:
    ~SdSource() override { Close(); }
    void Close() {
        if (file_)
            std::fclose(file_);
        file_ = nullptr;
        size_ = 0;
    }
    bool Open(const char* path) {
        Close();
        file_ = std::fopen(path, "rb");
        if (!file_)
            return false;
        std::setvbuf(file_, nullptr, _IONBF, 0);
        if (std::fseek(file_, 0, SEEK_END))
            return false;
        long n = std::ftell(file_);
        if (n < 0)
            return false;
        size_ = n;
        return true;
    }
    uint64_t Size() const override { return size_; }
    size_t ReadAt(uint32_t off, uint8_t* out, size_t n) override {
        if (!file_ || std::fseek(file_, off, SEEK_SET))
            return 0;
        return std::fread(out, 1, n, file_);
    }
    uint64_t NowMs() const override { return esp_timer_get_time() / 1000; }

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
};
lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius = 0,
              bool opaque = true) {
    auto* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opaque ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
lv_obj_t* Art(lv_obj_t* parent, int x, int y) {
    auto* o = lv_image_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_image_set_pivot(o, 0, 0);
    lv_image_set_scale_x(o, 347);
    lv_image_set_scale_y(o, 347);
    lv_image_set_antialias(o, true);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
}  // namespace

// Singleton reader and PSRAM pool live until reboot. No LVGL object or audio API
// is accessed here. Disabling/destroying the view only cancels requests; a blocked
// filesystem call may finish later without touching freed UI memory.
class Tab5SdSceneReader {
public:
    static Tab5SdSceneReader* Create() {
        static Tab5SdSceneReader* instance = nullptr;
        if (instance)
            return instance;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2 * kBytes + 2 * 1024 * 1024)
            return nullptr;
        const int64_t pool_started = esp_timer_get_time();
        void* mem =
            heap_caps_malloc(sizeof(Tab5SdSceneReader), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        auto* a =
            static_cast<uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        auto* b =
            static_cast<uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!mem || !a || !b) {
            heap_caps_free(mem);
            heap_caps_free(a);
            heap_caps_free(b);
            return nullptr;
        }
        const uint32_t pool_us = esp_timer_get_time() - pool_started;
        auto* self = new (mem) Tab5SdSceneReader(a, b);
        if (xTaskCreate(Run, "nabo_sd", 6144, self, 1, nullptr) != pdPASS) {
            self->~Tab5SdSceneReader();
            heap_caps_free(mem);
            heap_caps_free(a);
            heap_caps_free(b);
            return nullptr;
        }
        instance = self;
        ESP_LOGI("NaboSD", "pool=%u index=%u psram_free=%u internal_free=%u", unsigned(2 * kBytes),
                 unsigned(sizeof(nabo_sd::Pack)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        ESP_LOGI("NaboSDPerf",
                 "pool_alloc_us=%u trace_stack_bytes=%u read_deadline_ms=80 "
                 "crc_deadline_ms=80 chunk_bytes=%u",
                 unsigned(pool_us), unsigned(sizeof(nabo_sd::SceneMailbox::PumpTrace)),
                 unsigned(nabo_sd::kChunk));
        return instance;
    }
    nabo_sd::SceneMailbox mailbox;
    bool Failed(unsigned clip) const { return failures_.load() & (1u << clip); }

private:
    Tab5SdSceneReader(uint8_t* a, uint8_t* b) : mailbox(a, b, kBytes) {}
    static void Run(void* p) { static_cast<Tab5SdSceneReader*>(p)->Loop(); }
    void Loop() {
        int open_clip = -1;
        uint64_t reported = 0;
        uint32_t frames = 0, max_us = 0, busy = 0, sampled = 0;
        for (;;) {
            // Mounting runs asynchronously after Wi-Fi claims the SDMMC host.
            // Before it completes, keep the static face without poisoning a clip.
            const uint32_t token = Tab5SdReady() ? mailbox.Requested() : 0;
            if (token) {
                unsigned clip = nabo_sd::SceneMailbox::Clip(token);
                if (clip < 4 && !Failed(clip)) {
                    const int64_t begin = esp_timer_get_time();
                    nabo_sd::Error e = nabo_sd::Error::Ok;
                    nabo_sd::SceneMailbox::PumpTrace trace;
                    trace.frame.clock_us = []() { return uint64_t(esp_timer_get_time()); };
                    uint32_t open_us = 0;
                    if (open_clip != int(clip)) {
                        open_clip = -1;
                        sampled = 0;
                        if (!source_.Open(kPaths[clip]))
                            e = nabo_sd::Error::Io;
                        else
                            e = pack_.Open(source_, kBytes, 80, true);
                        if (e == nabo_sd::Error::Ok && (pack_.Count() != kCounts[clip] ||
                                                        pack_.Duration() != kCounts[clip] * 40 ||
                                                        pack_.MaxPayload() != kBytes))
                            e = nabo_sd::Error::Format;
                        if (e == nabo_sd::Error::Ok)
                            for (unsigned i = 0; i < pack_.Count(); ++i) {
                                const auto f = pack_.At(i);
                                if (f.x || f.y || f.width != 320 || f.height != 412 ||
                                    f.bytes != kBytes || f.at_ms != i * 40) {
                                    e = nabo_sd::Error::Format;
                                    break;
                                }
                            }
                        open_us = esp_timer_get_time() - begin;
                        if (e == nabo_sd::Error::Ok) {
                            open_clip = clip;
                            ESP_LOGI("NaboSD", "opened clip=%u frames=%u duration_ms=%u", clip,
                                     unsigned(pack_.Count()), unsigned(pack_.Duration()));
                        }
                    }
                    if (e == nabo_sd::Error::Ok) {
                        e = mailbox.Pump(pack_, token, &trace);
                        if (trace.published)
                            ++frames;
                        if (trace.busy)
                            ++busy;
                    }
                    const uint32_t total_us = esp_timer_get_time() - begin;
                    max_us = std::max(max_us, total_us);
                    if (trace.attempted && (sampled++ < 3 || (e != nabo_sd::Error::Ok &&
                                                              e != nabo_sd::Error::Cancelled))) {
                        const auto& t = trace.frame;
                        const char* phase = t.phase == nabo_sd::FrameTrace::ReadPhase  ? "read"
                                            : t.phase == nabo_sd::FrameTrace::CrcPhase ? "crc"
                                            : t.phase == nabo_sd::FrameTrace::Complete ? "done"
                                                                                       : "none";
                        ESP_LOGI("NaboSDPerf",
                                 "clip=%u frame=%u result=%u phase=%s open_us=%u "
                                 "total_us=%u claim_us=%u publish_us=%u submitted=%u",
                                 clip, unsigned(token & 255), unsigned(e), phase, unsigned(open_us),
                                 unsigned(total_us), unsigned(trace.claim_us),
                                 unsigned(trace.publish_us), unsigned(trace.published));
                        ESP_LOGI("NaboSDPerf",
                                 "read_bytes=%u read_chunks=%u read_us=%u "
                                 "read_max_us=%u crc_bytes=%u crc_chunks=%u "
                                 "crc_us=%u crc_max_us=%u",
                                 unsigned(t.read_bytes), unsigned(t.read_chunks),
                                 unsigned(t.read_us), unsigned(t.read_max_us),
                                 unsigned(t.crc_bytes), unsigned(t.crc_chunks), unsigned(t.crc_us),
                                 unsigned(t.crc_max_us));
                    }
                    if (e != nabo_sd::Error::Ok && e != nabo_sd::Error::Cancelled) {
                        failures_.fetch_or(1u << clip);
                        source_.Close();
                        open_clip = -1;
                        ESP_LOGW("NaboSD", "clip=%u error=%u; static fallback until reboot", clip,
                                 unsigned(e));
                    }
                }
            }
            const uint64_t now = source_.NowMs();
            if (now - reported >= 10000) {
                reported = now;
                ESP_LOGI("NaboSD",
                         "pump=%u max_us=%u psram_min=%u internal_min=%u stack_free=%u busy=%u",
                         frames, max_us,
                         unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                         unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                         unsigned(uxTaskGetStackHighWaterMark(nullptr)), busy);
                frames = max_us = busy = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(4));
        }
    }
    SdSource source_;
    nabo_sd::Pack pack_;
    std::atomic<uint32_t> failures_{0};
};

Tab5SdScene::Tab5SdScene(lv_obj_t* parent) {
    root_ = Box(parent, 0, 0, 512, 558, 0x0d1b2b);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    Box(root_, 94, 340, 352, 222, 0x193b56, 28);
    Box(root_, 108, 354, 324, 185, 0x10273b, 16);
    auto* shadow = Box(root_, 123, 347, 291, 36, 0x01070e, 18);
    lv_obj_set_style_bg_opa(shadow, 155, 0);
    lv_obj_set_style_shadow_color(shadow, lv_color_hex(0x01070e), 0);
    lv_obj_set_style_shadow_opa(shadow, 155, 0);
    lv_obj_set_style_shadow_width(shadow, 14, 0);
    auto* body_clip = Box(root_, 108, 354, 324, 185, 0x183550, 0);
    body_ = Art(body_clip, -69, -354);
    Box(root_, 94, 340, 352, 24, 0x285272, 12);
    Box(root_, 109, 341, 322, 2, 0x587f98);
    auto* head_clip = Box(root_, 39, 0, 434, 423, 0, 0, false);
    head_ = Art(head_clip, 0, 0);
    eyes_ = lv_image_create(head_clip);
    mouth_ = lv_image_create(head_clip);
    lv_obj_remove_flag(eyes_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_CLICKABLE);
    Box(root_, 94, 360, 15, 187, 0x234a64, 7);
    Box(root_, 432, 360, 15, 187, 0x234a64, 7);
    Box(root_, 107, 367, 1, 166, 0x436e88);
    Box(root_, 432, 367, 1, 166, 0x436e88);
    Box(root_, 94, 534, 352, 11, 0x38617a, 3);
    Box(root_, 100, 534, 340, 2, 0x7796aa);
    Box(root_, 103, 544, 334, 14, 0x193b56, 8);
    for (auto& d : descriptors_) {
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_RGB565A8;
        d.header.w = 320;
        d.header.h = 412;
        d.header.stride = 640;
        d.data_size = kBytes;
    }
    Source(&nabo_sd_neutral);
    Face(0, 0, 255);
    reader_ = Tab5SdSceneReader::Create();
}
void Tab5SdScene::Source(const lv_image_dsc_t* dsc, nabo_sd::SceneMailbox::Lease lease) {
    if (source_ == dsc)
        return;
    if (current_.slot >= 0)
        retired_ |= 1u << current_.slot;
    current_ = lease;
    source_ = dsc;
    lv_image_set_src(head_, dsc);
    lv_image_set_src(body_, dsc);
}
void Tab5SdScene::Face(unsigned eyes, unsigned mouth, uint8_t opacity) {
    auto show = [](lv_obj_t* o, unsigned state, const nabo_sd_patch_t* patches) {
        if (!state) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        const auto& p = patches[std::min(state, 2u) - 1];
        if (lv_image_get_src(o) != p.image)
            lv_image_set_src(o, p.image);
        lv_obj_set_pos(o, p.x, p.y);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(eyes_, eyes, nabo_sd_eyes);
    show(mouth_, mouth, nabo_sd_mouth);
    lv_obj_set_style_opa(mouth_, opacity, 0);
}
bool Tab5SdScene::Tick(uint64_t now, Input input) {
    if (detached_)
        return false;
    const bool wake = was_sleeping_ && !input.sleeping;
    auto scene = controller_.Tick(
        now, {input.speaking, input.playback, input.sleeping && !was_sleeping_, wake});
    was_sleeping_ = input.sleeping;
    const bool hidden = input.hidden || input.gesture;
    if (hidden)
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    const auto selection = work_clock_.Apply(now, nabo_scene::Select(now, scene, input));
    const int clip = selection.clip;
    const unsigned frame = selection.frame;
    if (scene.generation != last_generation_ || clip != last_clip_) {
        ++epoch_;
        last_generation_ = scene.generation;
        last_clip_ = clip;
        request_since_ = now;
        last_frame_ms_ = 0;
    }
    if (reader_ && clip >= 0 && !reader_->Failed(clip)) {
        reader_->mailbox.Request(clip, frame, epoch_);
        auto next = reader_->mailbox.TakeReady();
        if (next.slot >= 0) {
            auto& d = descriptors_[next.slot];
            lv_image_cache_drop(&d);
            d.data = next.pixels;
            Source(&d, next);
            last_frame_ms_ = now;
        }
        if (now - (last_frame_ms_ ? last_frame_ms_ : request_since_) > 160)
            Source(&nabo_sd_neutral);
    } else {
        if (reader_) {
            reader_->mailbox.CancelAll();
        }
        Source(&nabo_sd_neutral);
    }
    // Actual audio preempts a moving wake immediately. The static neutral uses
    // the original playback/drain-gated face cadence, never synthetic audio.
    speech_face_.SetState(input.speaking ? tab5_home::State::Speaking : tab5_home::State::Idle,
                          uint32_t(now));
    const auto face =
        speech_face_.Sample(uint32_t(now), input.voice, input.speaking && input.playback);
    const bool static_face = source_ == &nabo_sd_neutral;
    Face(static_face ? (input.sleeping && !input.speaking ? 2 : face.eyes) : 0,
         static_face && input.speaking && input.playback ? face.mouth : 0, face.mouth_opa);
    return !hidden;
}
void Tab5SdScene::RenderReady() {
    if (!reader_)
        return;
    for (int i = 0; i < 2; ++i)
        if (retired_ & (1u << i)) {
            lv_image_cache_drop(&descriptors_[i]);
            reader_->mailbox.Release({i, nullptr, 0});
        }
    retired_ = 0;
}
void Tab5SdScene::Detach() {
    detached_ = true;
    if (reader_)
        reader_->mailbox.CancelAll();
    // Pool is singleton-lifetime. Never free buffers while render work may own them.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}
#endif
