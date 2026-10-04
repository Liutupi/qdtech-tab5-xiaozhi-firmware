#include "tab5_sd_scene.h"
#ifdef CONFIG_QDTECH_TAB5_SD_SCENE
#include <cstdio>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nabo_frame_composer.h"
#include "nabo_sd_static.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/lv_area_private.h"
#include "tab5_sd.h"

namespace {
constexpr size_t kBytes = 320 * 412 * 3;
// SDMMC DMA into PSRAM needs cache-line aligned address and length (P4 L2 line is
// 64B; 128 also covers the 128B option). Unaligned destinations make IDF fall
// back to one 512B sector per transaction plus a heap bounce buffer each time,
// which measured ~2.5-3.8MB/s and missed every frame deadline.
constexpr size_t kDmaAlign = 128;
constexpr size_t kSlotBytes = kBytes + kDmaAlign;  // DMA staging for one raw frame
using Layout = nabo_sd::ComposeLayout;
static_assert(Layout::kSrcBytes == kBytes, "compositor expects the 320x412 pack frame");
constexpr size_t kOutBytes = Layout::kOutBytes;  // composed opaque RGB565 slot
constexpr int kVideoX = 39;                       // head clip / image origin in root
// Transient slowness (boot load, SDIO Wi-Fi bursts) skips frames; only a run of
// timeouts disables the clip. Format/CRC/IO errors stay fatal immediately.
constexpr unsigned kMaxConsecutiveTimeouts = 8;
constexpr uint32_t kTimeoutBackoffMs = 250;
// Measured on device with aligned DMA: ~5MB/s, single 8KB calls stalling up to
// 170ms while SDIO Wi-Fi shares the controller. 64KB reads cut the call count
// 8x; budgets cover a stalled-but-progressing frame instead of discarding it.
constexpr size_t kReadChunk = 64 * 1024;
// kCallDeadlineMs bounds one blocking read call and, separately, the frame CRC.
constexpr uint32_t kCallDeadlineMs = 150, kReadDeadlineMs = 200, kFrameDeadlineMs = 260;
// The UI keeps the last video frame through gaps up to this long before the
// static face returns; before the first frame of a clip the old 160ms applies.
constexpr uint64_t kHoldVideoMs = 1000, kFirstFrameMs = 160;
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
        position_ = -1;
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
        position_ = n;
        return true;
    }
    uint64_t Size() const override { return size_; }
    size_t ReadAt(uint32_t off, uint8_t* out, size_t n) override {
        if (!file_)
            return 0;
        // Only this worker owns the FILE; consecutive chunks already have the
        // desired cursor. Timeline jumps still perform an absolute seek.
        if (position_ < 0 || uint64_t(position_) != off) {
            if (std::fseek(file_, off, SEEK_SET)) {
                position_ = -1;
                return 0;
            }
        }
        const size_t got = std::fread(out, 1, n, file_);
        position_ = std::ferror(file_) ? -1 : long(uint64_t(off) + got);
        return got;
    }
    uint64_t NowMs() const override { return esp_timer_get_time() / 1000; }

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
    long position_ = -1;
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
    // Only the static portrait uses these scaled layers (rendered on change).
    // Video frames are pre-composed on the reader task and blitted unscaled.
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
    static Tab5SdSceneReader* Create(const uint16_t* background, const uint8_t* overlay) {
        static Tab5SdSceneReader* instance = nullptr;
        if (instance)
            return instance;
        if (!background || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) <
                               2 * kOutBytes + kSlotBytes + 2 * 1024 * 1024)
            return nullptr;
        const int64_t pool_started = esp_timer_get_time();
        constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        void* mem = heap_caps_malloc(sizeof(Tab5SdSceneReader), caps);
        auto* a = static_cast<uint8_t*>(heap_caps_aligned_alloc(kDmaAlign, kOutBytes, caps));
        auto* b = static_cast<uint8_t*>(heap_caps_aligned_alloc(kDmaAlign, kOutBytes, caps));
        // Optional: a third slot lets reading continue while one frame is shown
        // and the next waits for the UI tick. Without it the scene still works.
        auto* c = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > kOutBytes + 4 * 1024 * 1024
                      ? static_cast<uint8_t*>(heap_caps_aligned_alloc(kDmaAlign, kOutBytes, caps))
                      : nullptr;
        auto* staging =
            static_cast<uint8_t*>(heap_caps_aligned_alloc(kDmaAlign, kSlotBytes, caps));
        if (!mem || !a || !b || !staging) {
            heap_caps_free(mem);
            heap_caps_free(a);
            heap_caps_free(b);
            heap_caps_free(c);
            heap_caps_free(staging);
            return nullptr;
        }
        const uint32_t pool_us = esp_timer_get_time() - pool_started;
        auto* self = new (mem) Tab5SdSceneReader(a, b, c, staging, background, overlay);
        // Priority 2: above timer/UDP housekeeping, below LVGL(4) and audio(5/8).
        if (xTaskCreate(Run, "nabo_sd", 6144, self, 2, nullptr) != pdPASS) {
            self->~Tab5SdSceneReader();
            heap_caps_free(mem);
            heap_caps_free(a);
            heap_caps_free(b);
            heap_caps_free(c);
            heap_caps_free(staging);
            return nullptr;
        }
        instance = self;
        ESP_LOGI("NaboSD", "pool=%u align=%u index=%u psram_free=%u internal_free=%u",
                 unsigned((c ? 3 : 2) * kOutBytes + kSlotBytes), unsigned(kDmaAlign),
                 unsigned(sizeof(nabo_sd::Pack)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        ESP_LOGI("NaboSDPerf",
                 "pool_alloc_us=%u trace_stack_bytes=%u read_deadline_ms=%u "
                 "chunk_deadline_ms=%u crc_deadline_ms=%u frame_deadline_ms=%u chunk_bytes=%u "
                 "crc_chunk_bytes=%u hold_ms=%u",
                 unsigned(pool_us), unsigned(sizeof(nabo_sd::SceneMailbox::PumpTrace)),
                 unsigned(kReadDeadlineMs), unsigned(kCallDeadlineMs), unsigned(kCallDeadlineMs),
                 unsigned(kFrameDeadlineMs), unsigned(kReadChunk), unsigned(nabo_sd::kChunk),
                 unsigned(kHoldVideoMs));
        return instance;
    }
    nabo_sd::SceneMailbox mailbox;
    bool Failed(unsigned clip) const { return failures_.load() & (1u << clip); }

private:
    Tab5SdSceneReader(uint8_t* a, uint8_t* b, uint8_t* c, uint8_t* staging,
                      const uint16_t* background, const uint8_t* overlay)
        : mailbox(a, b, kOutBytes, kDmaAlign, c), composer_(background, overlay) {
        mailbox.SetComposer(staging, kSlotBytes, &ComposeThunk, &composer_);
    }
    static void ComposeThunk(void* context, const uint8_t* raw, uint8_t* out) {
        static_cast<const nabo_sd::FrameComposer*>(context)->Compose(raw, out);
    }
    static void Run(void* p) { static_cast<Tab5SdSceneReader*>(p)->Loop(); }
    void Loop() {
        int open_clip = -1;
        uint64_t reported = 0;
        uint32_t frames = 0, max_us = 0, busy = 0, sampled = 0, timeouts = 0;
        unsigned timeout_clip = 4;
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
                        else {
                            e = pack_.Open(source_, kBytes, kCallDeadlineMs, true, kReadDeadlineMs,
                                           kFrameDeadlineMs);
                            pack_.SetReadChunk(kReadChunk);
                            pack_.SetVerifyOnce(true);
                        }
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
                        if (trace.published) {
                            ++frames;
                            timeouts = 0;
                        }
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
                                 "total_us=%u claim_us=%u compose_us=%u publish_us=%u "
                                 "submitted=%u",
                                 clip, unsigned(token & 255), unsigned(e), phase, unsigned(open_us),
                                 unsigned(total_us), unsigned(trace.claim_us),
                                 unsigned(trace.compose_us), unsigned(trace.publish_us),
                                 unsigned(trace.published));
                        ESP_LOGI("NaboSDPerf",
                                 "read_bytes=%u read_chunks=%u read_us=%u "
                                 "read_max_us=%u crc_bytes=%u crc_chunks=%u "
                                 "crc_us=%u crc_max_us=%u",
                                 unsigned(t.read_bytes), unsigned(t.read_chunks),
                                 unsigned(t.read_us), unsigned(t.read_max_us),
                                 unsigned(t.crc_bytes), unsigned(t.crc_chunks), unsigned(t.crc_us),
                                 unsigned(t.crc_max_us));
                    }
                    if (e == nabo_sd::Error::Timeout && clip != timeout_clip) {
                        timeout_clip = clip;
                        timeouts = 0;
                    }
                    if (e == nabo_sd::Error::Timeout &&
                        ++timeouts < kMaxConsecutiveTimeouts) {
                        // Keep the clip open; the UI shows the static face until a
                        // later frame arrives. Back off so a slow card cannot spin.
                        ESP_LOGW("NaboSD", "clip=%u timeout %u/%u; skipping frame", clip,
                                 unsigned(timeouts), kMaxConsecutiveTimeouts);
                        vTaskDelay(pdMS_TO_TICKS(kTimeoutBackoffMs));
                    } else if (e != nabo_sd::Error::Ok && e != nabo_sd::Error::Cancelled) {
                        timeouts = 0;
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
    nabo_sd::FrameComposer composer_;
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
    // Pre-composed video (head + windowed body over the static art). It sits
    // where the head layer was, so the posts/sill below still draw in front.
    video_ = lv_image_create(root_);
    lv_obj_set_pos(video_, kVideoX, 0);
    lv_obj_remove_flag(video_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(video_, LV_OBJ_FLAG_HIDDEN);
    Box(root_, 94, 360, 15, 187, 0x234a64, 7);
    Box(root_, 432, 360, 15, 187, 0x234a64, 7);
    Box(root_, 107, 367, 1, 166, 0x436e88);
    Box(root_, 432, 367, 1, 166, 0x436e88);
    Box(root_, 94, 534, 352, 11, 0x38617a, 3);
    Box(root_, 100, 534, 340, 2, 0x7796aa);
    Box(root_, 103, 544, 334, 14, 0x193b56, 8);
    live_.header.magic = LV_IMAGE_HEADER_MAGIC;
    live_.header.cf = LV_COLOR_FORMAT_RGB565;
    live_.header.w = Layout::kOutW;
    live_.header.h = Layout::kOutH;
    live_.header.stride = Layout::kOutW * 2;
    live_.data_size = kOutBytes;
    // Character layers have no source yet, so this captures only the static art.
    background_ = CaptureBackground();
    Source(&nabo_sd_neutral);
    Face(0, 0, 255);
    reader_ = Tab5SdSceneReader::Create(background_, overlay_);
}
void Tab5SdScene::SetDirectBlit(DirectBlit blit, void* context) {
    blit_ = blit;
    blit_context_ = context;
}
uint16_t* Tab5SdScene::CaptureBackground() {
    lv_obj_update_layout(root_);
    lv_draw_buf_t* snap = lv_snapshot_take(root_, LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        ESP_LOGW("NaboSD", "background snapshot failed; static portrait only");
        return nullptr;
    }
    // The snapshot adds the object's extra draw margin on every side.
    const int ext = (int(snap->header.w) - int(lv_obj_get_width(root_))) / 2;
    const uint32_t snap_w = snap->header.w, snap_h = snap->header.h;
    uint16_t* out = nullptr;
    if (int(snap->header.w) >= ext + kVideoX + Layout::kOutW &&
        int(snap->header.h) >= ext + Layout::kOutH)
        out = static_cast<uint16_t*>(
            heap_caps_malloc(kOutBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out)
        for (int y = 0; y < Layout::kOutH; ++y)
            std::memcpy(out + size_t(y) * Layout::kOutW,
                        snap->data + size_t(y + ext) * snap->header.stride + (ext + kVideoX) * 2,
                        size_t(Layout::kOutW) * 2);
    lv_draw_buf_destroy(snap);
    // Front decorations are the root children created after the video layer.
    // Snapshot again without them; differing pixels form the overlay mask that
    // lets composed frames already contain them (required for direct output).
    if (out) {
        const uint32_t first = lv_obj_get_index(video_) + 1, count = lv_obj_get_child_count(root_);
        for (uint32_t i = first; i < count; ++i)
            lv_obj_add_flag(lv_obj_get_child(root_, i), LV_OBJ_FLAG_HIDDEN);
        lv_draw_buf_t* base = lv_snapshot_take(root_, LV_COLOR_FORMAT_RGB565);
        for (uint32_t i = first; i < count; ++i)
            lv_obj_remove_flag(lv_obj_get_child(root_, i), LV_OBJ_FLAG_HIDDEN);
        if (base && base->header.w == snap_w && base->header.h == snap_h)
            overlay_ = static_cast<uint8_t*>(heap_caps_malloc(
                size_t(Layout::kOutW) * Layout::kOutH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        unsigned covered = 0;
        if (overlay_)
            for (int y = 0; y < Layout::kOutH; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(
                    base->data + size_t(y + ext) * base->header.stride + (ext + kVideoX) * 2);
                for (int x = 0; x < Layout::kOutW; ++x) {
                    const bool differs = row[x] != out[size_t(y) * Layout::kOutW + x];
                    overlay_[size_t(y) * Layout::kOutW + x] = differs;
                    covered += differs;
                }
            }
        if (base)
            lv_draw_buf_destroy(base);
        ESP_LOGI("NaboSD", "overlay mask %s covered=%u", overlay_ ? "captured" : "unavailable",
                 covered);
    }
    ESP_LOGI("NaboSD", "background %ux%u ext=%d %s", unsigned(snap_w), unsigned(snap_h), ext,
             out ? "captured" : "unavailable");
    return out;
}
void Tab5SdScene::Source(const lv_image_dsc_t* dsc) {
    if (source_ == dsc)
        return;
    // Leaving video: the hidden layer is not drawn again, but release only after
    // the render that removes it, as the LVGL path may still reference it.
    if (live_slot_ >= 0)
        retired_ |= 1u << live_slot_;
    live_slot_ = -1;
    source_ = dsc;
    lv_obj_add_flag(video_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(head_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(head_, dsc);
    lv_image_set_src(body_, dsc);
}
// LVGL renders synchronously in this task (LV_OS_NONE, one SW draw unit), so a
// frame swap here can never race a render that reads the previous buffer.
void Tab5SdScene::Present(nabo_sd::SceneMailbox::Lease next, uint64_t now) {
    const int previous = live_slot_;
    live_.data = next.pixels;
    lv_image_cache_drop(&live_);
    live_slot_ = next.slot;
    if (source_ != &live_) {
        // Entering video goes through LVGL once, replacing the static layers.
        if (previous >= 0)
            retired_ |= 1u << previous;
        source_ = &live_;
        lv_image_set_src(video_, &live_);
        lv_obj_remove_flag(video_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(head_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(body_, LV_OBJ_FLAG_HIDDEN);
        ++lvgl_frames_;
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(video_, &area);
    const int64_t started = esp_timer_get_time();
    if (blit_ && DirectSafe(area) && blit_(blit_context_, area, next.pixels)) {
        // Output is on screen and LVGL will only ever read the new buffer.
        if (previous >= 0 && reader_)
            reader_->mailbox.Release({previous, nullptr, 0});
        ++direct_frames_;
        const uint32_t spent = uint32_t(esp_timer_get_time() - started);
        direct_max_us_ = std::max(direct_max_us_, spent);
        direct_total_us_ += spent;
    } else {
        if (previous >= 0)
            retired_ |= 1u << previous;
        lv_obj_invalidate(video_);
        ++lvgl_frames_;
    }
    if (now - blit_reported_ms_ >= 10000) {
        ESP_LOGI("NaboSDBlit", "direct=%u lvgl=%u direct_avg_us=%u direct_max_us=%u",
                 unsigned(direct_frames_), unsigned(lvgl_frames_),
                 unsigned(direct_frames_ ? direct_total_us_ / direct_frames_ : 0),
                 unsigned(direct_max_us_));
        blit_reported_ms_ = now;
        direct_frames_ = lvgl_frames_ = direct_max_us_ = direct_total_us_ = 0;
    }
}
// Direct output skips LVGL, so it is allowed only when nothing outside the
// baked window art can cover the video: on the active screen, fully inside
// every ancestor, opaque, and no later sibling or top/system layer overlaps.
bool Tab5SdScene::DirectSafe(const lv_area_t& area) const {
    if (lv_obj_get_screen(video_) != lv_screen_active() || !lv_obj_is_visible(video_) ||
        lv_obj_get_style_opa_recursive(video_, LV_PART_MAIN) < LV_OPA_COVER)
        return false;
    auto overlaps = [&](lv_obj_t* parent, uint32_t from) {
        const uint32_t count = lv_obj_get_child_count(parent);
        for (uint32_t i = from; i < count; ++i) {
            lv_obj_t* sibling = lv_obj_get_child(parent, i);
            if (lv_obj_has_flag(sibling, LV_OBJ_FLAG_HIDDEN))
                continue;
            lv_area_t other;
            lv_obj_get_coords(sibling, &other);
            if (lv_area_is_on(&area, &other))
                return true;
        }
        return false;
    };
    for (lv_obj_t* obj = video_; lv_obj_get_parent(obj); obj = lv_obj_get_parent(obj)) {
        lv_obj_t* parent = lv_obj_get_parent(obj);
        lv_area_t bounds;
        lv_obj_get_coords(parent, &bounds);
        if (!lv_area_is_in(&area, &bounds, 0))
            return false;
        // Decorations above the video inside root_ are baked into each frame.
        if (parent != root_ && overlaps(parent, lv_obj_get_index(obj) + 1))
            return false;
    }
    return !overlaps(lv_layer_top(), 0) && !overlaps(lv_layer_sys(), 0);
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
            Present(next, now);
            last_frame_ms_ = now;
        }
        if (now - (last_frame_ms_ ? last_frame_ms_ : request_since_) >
            (last_frame_ms_ ? kHoldVideoMs : kFirstFrameMs))
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
    for (int i = 0; i < nabo_sd::SceneMailbox::kSlots; ++i)
        if (retired_ & (1u << i)) {
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
