#include "tab5_nabo_avatar.h"

#include "misc/cache/instance/lv_image_cache.h"
#include "nabo_avatar_frame.h"
#include "sdkconfig.h"

#if defined(CONFIG_QDTECH_TAB5_NATIVE_UI) && defined(CONFIG_QDTECH_TAB5_SD_SCENE)
#include <cstdio>
#include <mutex>
#include <new>
#include <optional>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nabo_scene_stream.h"
#include "tab5_sd.h"
#include "tab5_sd_scene.h"

namespace {
constexpr const char* kClips[] = {"idle", "A07", "E10", "E06", "A05", "P02"};
constexpr size_t kRawBytes = 320 * 412 * 3;
uint64_t NowMs() { return uint64_t(esp_timer_get_time() / 1000); }

class FileSource : public nabo_sd::Source {
public:
    ~FileSource() override { Close(); }
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
        const long end = std::ftell(file_);
        if (end < 0)
            return false;
        size_ = uint64_t(end);
        position_ = end;
        return true;
    }
    uint64_t Size() const override { return size_; }
    uint64_t NowMs() const override { return ::NowMs(); }
    size_t ReadAt(uint32_t offset, uint8_t* out, size_t bytes) override {
        if (!file_)
            return 0;
        if (position_ < 0 || uint64_t(position_) != offset) {
            if (std::fseek(file_, offset, SEEK_SET)) {
                position_ = -1;
                return 0;
            }
        }
        const size_t got = std::fread(out, 1, bytes, file_);
        position_ = std::ferror(file_) ? -1 : long(uint64_t(offset) + got);
        return got;
    }

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
    long position_ = -1;
};
}  // namespace

// The existing mailbox owns the leases. Only this worker reads/decodes; the UI
// takes completed thumbnails and retains their slots until rendering finishes.
class Tab5NaboAvatarReader {
public:
    using Lease = nabo_sd::SceneMailbox::Lease;
    static Tab5NaboAvatarReader* Get() {
        static Tab5NaboAvatarReader* instance = nullptr;
        if (instance)
            return instance;
        constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        void* memory = heap_caps_malloc(sizeof(Tab5NaboAvatarReader), caps);
        if (!memory)
            return nullptr;
        auto* self = new (memory) Tab5NaboAvatarReader;
        if (xTaskCreatePinnedToCoreWithCaps(Run, "nabo_avatar", 8192, self, 2, &self->task_, 1,
                                            caps) != pdPASS) {
            self->~Tab5NaboAvatarReader();
            heap_caps_free(memory);
            return nullptr;
        }
        instance = self;
        return instance;
    }
    void Want(int clip, bool active) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (clip == wanted_clip_ && active == active_)
                return;
            wanted_clip_ = clip;
            active_ = active;
            ++epoch_;
            if (mailbox_)
                mailbox_->CancelAll();
        }
        xTaskNotifyGive(task_);
    }
    Lease Take() {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_ && opened_epoch_ == epoch_ && mailbox_ ? mailbox_->TakeReady() : Lease{};
    }
    void Release(Lease lease) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mailbox_)
            mailbox_->Release(lease);
    }

private:
    static void Run(void* context) { static_cast<Tab5NaboAvatarReader*>(context)->Loop(); }
    static void Compose(void* context, const uint8_t* raw, uint8_t* out) {
        auto* self = static_cast<Tab5NaboAvatarReader*>(context);
        nabo_avatar::Compose(self->entry_, raw, self->width_, self->height_,
                             reinterpret_cast<uint16_t*>(out));
    }
    bool Borrow() {
        if (!Tab5SdSceneBorrowPool(&pool_))
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        mailbox_.emplace(pool_.slots[0], pool_.slots[1], pool_.slot_bytes, 128, pool_.slots[2]);
        mailbox_->SetComposer(pool_.staging, pool_.encoded_bytes, Compose, this);
        pack_.SetDecodeBuffer(pool_.encoded, pool_.encoded_bytes);
        return true;
    }
    void Return() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mailbox_.reset();
        }
        Tab5SdSceneReturnPool();
        pool_ = {};
    }
    bool Open(FileSource& source, int clip) {
        char path[96];
        std::snprintf(path, sizeof(path), "/sdcard/tab5/nabo/lively-v1/%s.nab", kClips[clip]);
        if (!source.Open(path) && (clip != 0 || !source.Open("/sdcard/tab5/nabo/idle.nab")))
            return false;
        uint8_t header[64];
        if (source.ReadAt(0, header, sizeof(header)) != sizeof(header))
            return false;
        width_ = nabo_sd::U16(header + 12);
        height_ = nabo_sd::U16(header + 14);
        if (width_ != 320 || (height_ != 349 && height_ != 412))
            return false;
        if (pack_.Open(source, kRawBytes, 300, true, 500, 600) != nabo_sd::Error::Ok)
            return false;
        pack_.SetReadChunk(64 * 1024);
        pack_.SetVerifyOnce(true);
        return true;
    }
    void Loop() {
        FileSource source;
        bool borrowed = false, opened = false;
        uint32_t epoch = 0;
        int clip = 0;
        uint64_t started = 0;
        nabo_sd::StreamRecovery recovery;
        for (;;) {
            bool active;
            uint32_t wanted_epoch;
            int wanted_clip;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active = active_;
                wanted_epoch = epoch_;
                wanted_clip = wanted_clip_;
            }
            if (!active) {
                source.Close();
                opened = false;
                if (borrowed) {
                    Return();
                    borrowed = false;
                    ESP_LOGI("NaboAvatar", "stopped; home buffers returned");
                }
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            if (!Tab5SdReady() || (!borrowed && !(borrowed = Borrow()))) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
                continue;
            }
            if (epoch != wanted_epoch) {
                epoch = wanted_epoch;
                clip = wanted_clip;
                opened = false;
            }
            if (opened && clip && NowMs() - started >= pack_.Duration()) {
                clip = 0;  // One mood gesture, then the natural idle loop.
                opened = false;
            }
            if (!opened) {
                opened = Open(source, clip);
                if (!opened && clip) {
                    clip = 0;
                    opened = Open(source, clip);
                }
                if (!opened) {
                    source.Close();
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
                    continue;
                }
                started = NowMs();
                recovery = {};
                ESP_LOGI("NaboAvatar", "playing %s at %dx%d", kClips[clip], width_, height_);
            }
            const uint64_t now = NowMs();
            if (recovery.Ready(now)) {
                const unsigned frame = pack_.Due(now - started);
                uint32_t token;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!active_ || epoch != epoch_)
                        continue;
                    entry_ = pack_.At(frame);
                    opened_epoch_ = epoch;
                    mailbox_->Request(unsigned(clip), frame, epoch);
                    token = mailbox_->Requested();
                }
                const auto error = mailbox_->Pump(pack_, token);
                recovery.Result(error, NowMs(), true);
                if (recovery.Failed()) {
                    // Retry from idle while the UI retains its last usable portrait.
                    ESP_LOGW("NaboAvatar", "clip %s failed (%d)", kClips[clip], int(error));
                    clip = 0;
                    opened = false;
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
                }
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        }
    }

    std::mutex mutex_;
    std::optional<nabo_sd::SceneMailbox> mailbox_;
    Tab5SdPoolLease pool_{};
    nabo_sd::Pack pack_;
    nabo_sd::Entry entry_{};
    TaskHandle_t task_ = nullptr;
    uint32_t epoch_ = 0, opened_epoch_ = 0;
    int wanted_clip_ = 0, width_ = 0, height_ = 0;
    bool active_ = false;
};
#endif

LV_IMAGE_DECLARE(nabo_push_avatar);

Tab5NaboAvatar::Tab5NaboAvatar(lv_obj_t* parent) {
    image_ = lv_image_create(parent);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(image_, &nabo_push_avatar);
    lv_obj_set_pos(image_, 37, 0);
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<Tab5NaboAvatar*>(lv_timer_get_user_data(timer))->Tick();
        },
        40, this);
    lv_timer_pause(timer_);
    display_ = lv_obj_get_display(parent);
    lv_display_add_event_cb(display_, RenderReady, LV_EVENT_RENDER_READY, this);
}

Tab5NaboAvatar::~Tab5NaboAvatar() {
    Stop();
    lv_timer_delete(timer_);
    lv_display_remove_event_cb_with_user_data(display_, RenderReady, this);
    lv_obj_delete(image_);
}

void Tab5NaboAvatar::SetMood(const std::string& mood) {
    const int next = mood == "alert"      ? 1
                     : mood == "rain"     ? 2
                     : mood == "happy"    ? 3
                     : mood == "thinking" ? 4
                     : mood == "idle"     ? 5
                                          : 0;
    if (clip_ == next)
        return;
    clip_ = next;
#if defined(CONFIG_QDTECH_TAB5_NATIVE_UI) && defined(CONFIG_QDTECH_TAB5_SD_SCENE)
    if (active_ && reader_)
        reader_->Want(clip_, true);
#endif
}

void Tab5NaboAvatar::Start() {
    if (!active_) {
        shown_frames_ = 0;
        reported_ms_ = lv_tick_get();
    }
    active_ = true;
#if defined(CONFIG_QDTECH_TAB5_NATIVE_UI) && defined(CONFIG_QDTECH_TAB5_SD_SCENE)
    if (!reader_)
        reader_ = Tab5NaboAvatarReader::Get();
    if (reader_)
        reader_->Want(clip_, true);
#endif
    lv_timer_resume(timer_);
    lv_timer_ready(timer_);
}

void Tab5NaboAvatar::Stop() {
    active_ = false;
    lv_timer_pause(timer_);
    lv_image_set_src(image_, &nabo_push_avatar);
    lv_obj_set_pos(image_, 37, 0);
    lv_image_cache_drop(&frame_);
#if defined(CONFIG_QDTECH_TAB5_NATIVE_UI) && defined(CONFIG_QDTECH_TAB5_SD_SCENE)
    if (reader_) {
        reader_->Release(shown_);
        reader_->Release(retired_);
        reader_->Want(clip_, false);
    }
#endif
    shown_ = retired_ = {};
}

void Tab5NaboAvatar::Tick() {
    if (!active_ || !lv_obj_is_visible(image_)) {
        Stop();  // Also catches a parent page hidden without calling this page's Hide().
        return;
    }
#if defined(CONFIG_QDTECH_TAB5_NATIVE_UI) && defined(CONFIG_QDTECH_TAB5_SD_SCENE)
    if (!reader_)
        return;
    const auto next = reader_->Take();
    if (next.slot < 0)
        return;
    reader_->Release(retired_);
    retired_ = shown_;
    shown_ = next;
    lv_image_cache_drop(&frame_);
    frame_.header.magic = LV_IMAGE_HEADER_MAGIC;
    frame_.header.cf = LV_COLOR_FORMAT_RGB565;
    frame_.header.w = nabo_avatar::kWidth;
    frame_.header.h = nabo_avatar::kHeight;
    frame_.header.stride = nabo_avatar::kWidth * 2;
    frame_.data_size = nabo_avatar::kWidth * nabo_avatar::kHeight * 2;
    frame_.data = next.pixels;
    lv_image_set_src(image_, &frame_);
    lv_obj_set_pos(image_, (208 - nabo_avatar::kWidth) / 2, 0);
    lv_obj_invalidate(image_);
    ++shown_frames_;
    const uint32_t now = lv_tick_get();
    if (now - reported_ms_ >= 10000) {
        ESP_LOGI("NaboAvatar", "presented %u frames in %u ms", unsigned(shown_frames_),
                 unsigned(now - reported_ms_));
        reported_ms_ = now;
        shown_frames_ = 0;
    }
#endif
}

void Tab5NaboAvatar::RenderReady(lv_event_t* event) {
#if defined(CONFIG_QDTECH_TAB5_NATIVE_UI) && defined(CONFIG_QDTECH_TAB5_SD_SCENE)
    auto* self = static_cast<Tab5NaboAvatar*>(lv_event_get_user_data(event));
    if (self->reader_)
        self->reader_->Release(self->retired_);
    self->retired_ = {};
#else
    (void)event;
#endif
}
