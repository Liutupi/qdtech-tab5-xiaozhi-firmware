#include "tab5_radio_stage.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "nabo_framepack.h"
#include "sdkconfig.h"
#include "src/misc/lv_area_private.h"
#include "tab5_sd.h"
#ifdef CONFIG_QDTECH_TAB5_SD_SCENE
#include "tab5_sd_scene.h"
#endif

LV_FONT_DECLARE(qd_font_cjk_28);

namespace {

const char* TAG = "RadioStage";
constexpr const char* kDir = "/sdcard/tab5/nabo/radio-v1";
// Packs are 451x395 (94% of the source clips) so the largest frame, 534 KB as
// RGB565A8, fits a 571 KB slot of the home scene's pool.
constexpr int kCanvasW = 451, kCanvasH = 395;
constexpr int kNaboX = 58, kNaboY = 123;  // canvas bottom sits on the desk edge
constexpr size_t kReadChunk = 64 * 1024;  // fewer, longer SD transfers
// Stage geometry shared by the LVGL objects and the reader's composed frames.
constexpr int kStageW = 568, kDeskTop = 518;
constexpr int kArmX0 = 478, kArmY0 = -4, kArmX1 = 506, kArmY1 = 262, kArmWidth = 6;
constexpr int kMicRight = 14, kMicY = 250, kMugX = 26;
int MicX(int width) { return kStageW - kMicRight - width; }
int MugY(int height) { return kDeskTop + 6 - height / 2; }
constexpr int kSlots = 3;
constexpr int kProps = 2;
const char* const kPropNames[kProps] = {"mic", "mug"};

// Palette: 午夜琥珀 from the AI Passport NABO FM design.
constexpr uint32_t kStage = 0x111725, kStageLine = 0x161e2f, kStageDot = 0x1a2335, kDesk = 0x1d2433,
                   kDeskHi = 0x2c3549, kAmber = 0xf7b955, kDialBg = 0x171107, kRed = 0xff3b30,
                   kRedOff = 0x341c1f, kMicArm = 0x556079, kMuted = 0x8b93a5;

uint64_t NowMs() { return esp_timer_get_time() / 1000; }

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
        const long n = std::ftell(file_);
        if (n < 0)
            return false;
        size_ = uint64_t(n);
        position_ = n;
        return true;
    }
    uint64_t Size() const override { return size_; }
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
    uint64_t NowMs() const override { return ::NowMs(); }

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
    long position_ = -1;
};

void Path(char* out, size_t size, const char* stem) {
    std::snprintf(out, size, "%s/%s.nab", kDir, stem);
}

void FillDsc(lv_image_dsc_t* dsc, const uint8_t* pixels, int w, int h, bool opaque = false) {
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;  // RGB565 plane; RGB565A8 appends the alpha plane
    dsc->data_size = uint32_t(w) * h * (opaque ? 2 : 3);
    dsc->data = pixels;
}

uint16_t Rgb565(uint32_t rgb) {
    return uint16_t(((rgb >> 19) & 0x1f) << 11 | ((rgb >> 10) & 0x3f) << 5 | ((rgb >> 3) & 0x1f));
}

// The wall behind NABO, in stage coordinates. Must match the LVGL wall: dots
// from DrawFoam (4 px squares, 18 px apart, offset 7) and the 2 px seams.
uint16_t WallPixel(int sx, int sy, uint32_t stage, uint32_t line, uint32_t dot) {
    if (sx % 142 == 140 || sx % 142 == 141)
        return Rgb565(line);
    const bool on_dot = sx >= 7 && sy >= 7 && (sx - 7) % 18 < 4 && (sy - 7) % 18 < 4;
    return Rgb565(on_dot ? dot : stage);
}

uint16_t Blend565(uint16_t s, uint16_t d, unsigned k) {
    if (k == 255)
        return s;
    const unsigned j = 255 - k;
    const unsigned r = (((s >> 11) * k + (d >> 11) * j) / 255) & 31;
    const unsigned g = ((((s >> 5) & 63) * k + ((d >> 5) & 63) * j) / 255) & 63;
    const unsigned b = (((s & 31) * k + (d & 31) * j) / 255) & 31;
    return uint16_t(r << 11 | g << 5 | b);
}

bool OnArm(int sx, int sy) {
    // Distance from the microphone stand's centre line, squared, against its half width.
    const float dx = kArmX1 - kArmX0, dy = kArmY1 - kArmY0;
    float t = ((sx - kArmX0) * dx + (sy - kArmY0) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float ex = sx - (kArmX0 + t * dx), ey = sy - (kArmY0 + t * dy);
    return ex * ex + ey * ey <= (kArmWidth / 2.0f) * (kArmWidth / 2.0f);
}

}  // namespace

struct RadioFrame {
    const uint8_t* pixels = nullptr;
    uint16_t x = 0, y = 0, w = 0, h = 0;
};

// One worker reads one action pack at a time into frame buffers borrowed from
// the home scene, which is hidden whenever the radio page shows. Slots move
// Free -> Writing -> Ready (worker) -> Shown -> Retired (UI) -> Free.
class Tab5RadioStageReader {
public:
    enum Status { Scanning, Ready, NoCard, NoPacks, NoMemory };

    static Tab5RadioStageReader* Get() {
        static Tab5RadioStageReader* instance = nullptr;
        if (instance)
            return instance;
        constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        void* memory = heap_caps_malloc(sizeof(Tab5RadioStageReader), caps);
        if (!memory)
            return nullptr;
        auto* self = new (memory) Tab5RadioStageReader();
        // PSRAM stack: this task only touches the SD card, never flash.
        if (xTaskCreatePinnedToCoreWithCaps(Run, "radio_stage", 8192, self, 2, &self->task_, 1,
                                            caps) != pdPASS) {
            ESP_LOGW(TAG, "reader task unavailable");
            self->~Tab5RadioStageReader();
            heap_caps_free(memory);
            return nullptr;
        }
        instance = self;
        return instance;
    }

    void Want(int action, bool active, bool restart = false) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (action == want_action_ && active == want_active_ && !restart)
                return;
            if (active && !want_active_)
                rescan_.store(true, std::memory_order_release);
            if (action != want_action_ || !want_active_ || restart) {
                want_start_ = NowMs();
                epoch_.fetch_add(1, std::memory_order_acq_rel);
                // A queued frame of the previous action must not appear after the switch.
                for (auto& slot : slots_)
                    if (slot.state == SlotReady)
                        slot.state = SlotFree;
            }
            want_action_ = action;
            want_active_ = active;
        }
        xTaskNotifyGive(task_);
    }

    bool TakeReady(RadioFrame* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot* ready = nullptr;
        for (auto& slot : slots_)
            if (slot.state == SlotReady && (!ready || slot.seq > ready->seq))
                ready = &slot;
        if (!ready)
            return false;
        for (auto& slot : slots_) {
            if (slot.state == SlotRetired)
                slot.state = SlotFree;  // replaced before any render read it
            else if (slot.state == SlotShown)
                slot.state = SlotRetired;
        }
        ready->state = SlotShown;
        *out = {ready->pixels, ready->x, ready->y, ready->w, ready->h};
        return true;
    }

    void ReleaseRetired() {
        bool released = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& slot : slots_)
                if (slot.state == SlotRetired) {
                    slot.state = SlotFree;
                    released = true;
                }
        }
        if (released)
            xTaskNotifyGive(task_);
    }

    Status GetStatus() const { return status_.load(std::memory_order_acquire); }
    uint32_t DurationMs(int action) const {
        return action >= 0 && action < nabo_radio::kActionCount ? durations_[action] : 0;
    }
    bool Prop(int index, RadioFrame* out) const {
        if (!props_ready_.load(std::memory_order_acquire) || index < 0 || index >= kProps ||
            !props_[index].pixels)
            return false;
        *out = props_[index];
        return true;
    }

private:
    enum SlotState : uint8_t { SlotFree, SlotWriting, SlotReady, SlotShown, SlotRetired };
    struct Slot {
        uint8_t* pixels = nullptr;
        uint16_t x = 0, y = 0, w = 0, h = 0;
        uint32_t seq = 0;
        SlotState state = SlotFree;
    };

    static void Run(void* arg) { static_cast<Tab5RadioStageReader*>(arg)->Loop(); }

    // Wait for the card, then check every pack header and decode the props.
    void Scan() {
        while (!Tab5SdReady()) {
            status_.store(NoCard, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        int present = 0;
        char path[96];
        for (int i = 0; i < nabo_radio::kActionCount; ++i) {
            Path(path, sizeof(path), nabo_radio::ActionName(static_cast<nabo_radio::Action>(i)));
            FILE* file = std::fopen(path, "rb");
            uint8_t header[64];
            if (file && std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
                !std::memcmp(header, "NABOSD1\0", 8) &&
                nabo_sd::Crc(header, 60) == nabo_sd::U32(header + 60) &&
                nabo_sd::U16(header + 12) == kCanvasW && nabo_sd::U16(header + 14) == kCanvasH) {
                durations_[i] = nabo_sd::U32(header + 24);
                ++present;
            } else {
                ESP_LOGW(TAG, "missing or old %s", path);
            }
            if (file)
                std::fclose(file);
        }
        for (int i = 0; i < kProps; ++i)
            if (!props_[i].pixels)
                LoadProp(i);
        props_ready_.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "%d/%d action packs on the card", present, nabo_radio::kActionCount);
        status_.store(present ? Ready : NoPacks, std::memory_order_release);
    }

    // Props are small: they get their own memory, decoded once.
    void LoadProp(int index) {
        char path[96];
        Path(path, sizeof(path), kPropNames[index]);
        FileSource source;
        // pack_ is free here: Scan runs before any action is opened. A Pack holds an
        // 8 KB frame index and must never live on this task's stack.
        nabo_sd::Pack& pack = pack_;
        if (!source.Open(path) || pack.Open(source, 64 * 1024, 400, true) != nabo_sd::Error::Ok)
            return;
        const nabo_sd::Entry entry = pack.At(0);
        const size_t bytes = size_t(entry.width) * entry.height * 3;
        constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        auto* encoded = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, entry.bytes, caps));
        auto* pixels = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, bytes, caps));
        if (encoded && pixels) {
            pack.SetDecodeBuffer(encoded, entry.bytes);
            if (pack.Frame(0, pixels, bytes) == nabo_sd::Error::Ok) {
                props_[index] = {pixels, entry.x, entry.y, entry.width, entry.height};
                pixels = nullptr;
            }
        }
        heap_caps_free(encoded);
        heap_caps_free(pixels);
    }

    bool Borrow(bool log_refusal) {
#ifdef CONFIG_QDTECH_TAB5_SD_SCENE
        Tab5SdPoolLease lease{};
        if (!Tab5SdSceneBorrowPool(&lease, log_refusal))
            return false;
        if (!lease.slots[2]) {
            // Two composed slots plus one raw frame are needed.
            Tab5SdSceneReturnPool();
            if (log_refusal)
                ESP_LOGW(TAG, "home pool has only two slots");
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < kSlots; ++i)
            slots_[i] = {i < 2 ? lease.slots[i] : nullptr, 0, 0, 0, 0, 0, SlotFree};
        raw_ = lease.slots[2];
        background_ = reinterpret_cast<uint16_t*>(lease.staging);
        background_ready_ = false;
        slot_bytes_ = lease.slot_bytes;
        encoded_ = lease.encoded;
        encoded_bytes_ = lease.encoded_bytes;
        return true;
#else
        (void)log_refusal;
        return false;
#endif
    }

    void GiveBack() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& slot : slots_)
                slot = {};
            encoded_ = nullptr;
            raw_ = nullptr;
            background_ = nullptr;
        }
#ifdef CONFIG_QDTECH_TAB5_SD_SCENE
        Tab5SdSceneReturnPool();
#endif
    }

    void Loop() {
        Scan();
        FileSource source;
        uint32_t open_epoch = 0;
        bool opened = false, one_shot = false, borrowed = false;
        int last_frame = -1, borrow_tries = 0;
        for (;;) {
            int action;
            bool active;
            uint64_t start;
            const uint32_t epoch = epoch_.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                action = want_action_;
                active = want_active_;
                start = want_start_;
            }
            // The card may have been filled since the last look; check once per show.
            if (active && GetStatus() == NoPacks && rescan_.exchange(false))
                Scan();
            if (!active || GetStatus() == NoCard || GetStatus() == NoPacks) {
                if (opened)
                    source.Close();
                opened = false;
                open_epoch = 0;
                if (borrowed) {
                    GiveBack();
                    borrowed = false;
                }
                borrow_tries = 0;
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            if (!borrowed) {
                // The home scene hands over its pool once its last frame has been drawn.
                borrowed = Borrow(borrow_tries % 25 == 24);
                if (!borrowed) {
                    if (++borrow_tries == 25)
                        status_.store(NoMemory, std::memory_order_release);
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
                    continue;
                }
                if (GetStatus() == NoMemory)
                    status_.store(Ready, std::memory_order_release);
                open_epoch = 0;
            }
            if (epoch != open_epoch) {
                open_epoch = epoch;
                last_frame = -1;
                one_shot = nabo_radio::IsOneShot(static_cast<nabo_radio::Action>(action));
                char path[96];
                Path(path, sizeof(path),
                     nabo_radio::ActionName(static_cast<nabo_radio::Action>(action)));
                pack_.SetDecodeBuffer(encoded_, encoded_bytes_);
                pack_.SetVerifyOnce(true);
                opened = source.Open(path) &&
                         pack_.Open(source, slot_bytes_, 300, true, 500, 600) == nabo_sd::Error::Ok;
                pack_.SetReadChunk(kReadChunk);
                if (!opened)
                    ESP_LOGW(TAG, "cannot open %s", path);
            }
            uint32_t wait_ms = 1000;
            if (opened) {
                const uint64_t elapsed = NowMs() - start;
                const uint32_t duration = pack_.Duration();
                const uint32_t frame =
                    one_shot && elapsed >= duration ? pack_.Count() - 1 : pack_.Due(elapsed);
                if (int(frame) != last_frame) {
                    if (Decode(frame, epoch))
                        last_frame = int(frame);
                    else
                        wait_ms = 10;
                }
                if (wait_ms != 10) {
                    const uint32_t phase = uint32_t(elapsed % duration);
                    const uint32_t next =
                        frame + 1 < pack_.Count() ? pack_.At(frame + 1).at_ms : duration;
                    wait_ms = std::clamp<uint32_t>(next > phase ? next - phase : 5, 5, 250);
                    if (one_shot && elapsed >= duration)
                        wait_ms = 1000;
                }
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        }
    }

    // Returns false when the frame should be tried again shortly.
    bool Decode(uint32_t frame, uint32_t epoch) {
        Slot* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& candidate : slots_)
                if (candidate.pixels && candidate.state == SlotFree) {
                    slot = &candidate;
                    slot->state = SlotWriting;
                    break;
                }
        }
        if (!slot)
            return false;
        const nabo_sd::Entry entry = pack_.At(frame);
        const nabo_sd::Error error = pack_.Frame(frame, raw_, slot_bytes_, {&epoch_, epoch});
        if (error == nabo_sd::Error::Ok)
            Compose(entry, raw_, reinterpret_cast<uint16_t*>(slot->pixels));
        std::lock_guard<std::mutex> lock(mutex_);
        if (error != nabo_sd::Error::Ok || epoch_.load(std::memory_order_acquire) != epoch) {
            slot->state = SlotFree;
            if (error != nabo_sd::Error::Ok && error != nabo_sd::Error::Cancelled)
                ESP_LOGW(TAG, "frame %u failed (%d)", unsigned(frame), int(error));
            // A slow card or a read error is retried; a corrupt frame is skipped.
            return error != nabo_sd::Error::Timeout && error != nabo_sd::Error::Io;
        }
        for (auto& other : slots_)
            if (other.state == SlotReady)
                other.state = SlotFree;  // only the newest frame is worth showing
        *slot = {slot->pixels, 0, 0, kCanvasW, kCanvasH, ++seq_, SlotReady};
        return true;
    }

    // Wall and microphone stand, then NABO (RGB565A8, cropped), then the mic and
    // mug in front, into an opaque full-canvas RGB565 frame. Done here, on the
    // reader's core, so the UI only copies pixels to the panel.
    void Compose(const nabo_sd::Entry& e, const uint8_t* raw, uint16_t* out) {
        if (!background_ready_) {
            for (int y = 0; y < kCanvasH; ++y)
                for (int x = 0; x < kCanvasW; ++x) {
                    const int sx = kNaboX + x, sy = kNaboY + y;
                    background_[y * kCanvasW + x] =
                        OnArm(sx, sy) ? Rgb565(kMicArm)
                                      : WallPixel(sx, sy, kStage, kStageLine, kStageDot);
                }
            background_ready_ = true;
        }
        std::memcpy(out, background_, size_t(kCanvasW) * kCanvasH * 2);
        Blend(out, raw, e.x, e.y, e.width, e.height);
        if (props_ready_.load(std::memory_order_acquire)) {
            const RadioFrame& mic = props_[0];
            const RadioFrame& mug = props_[1];
            if (mic.pixels)
                Blend(out, mic.pixels, MicX(mic.w) - kNaboX, kMicY - kNaboY, mic.w, mic.h);
            if (mug.pixels)
                Blend(out, mug.pixels, kMugX - kNaboX, MugY(mug.h) - kNaboY, mug.w, mug.h);
        }
    }

    // Alpha-blends an RGB565A8 image at canvas position (x, y), clipped to the canvas.
    static void Blend(uint16_t* out, const uint8_t* image, int x, int y, int w, int h) {
        const auto* rgb = reinterpret_cast<const uint16_t*>(image);
        const uint8_t* alpha = image + size_t(w) * h * 2;
        const int x0 = std::max(0, -x), x1 = std::min(w, kCanvasW - x);
        const int y0 = std::max(0, -y), y1 = std::min(h, kCanvasH - y);
        for (int row = y0; row < y1; ++row) {
            uint16_t* dst = out + size_t(y + row) * kCanvasW + x;
            const uint16_t* src = rgb + size_t(row) * w;
            const uint8_t* a = alpha + size_t(row) * w;
            for (int col = x0; col < x1; ++col)
                if (a[col])
                    dst[col] = Blend565(src[col], dst[col], a[col]);
        }
    }

    mutable std::mutex mutex_;
    Slot slots_[kSlots];
    size_t slot_bytes_ = 0;
    uint8_t* raw_ = nullptr;          // one decoded RGB565A8 frame before composing
    uint16_t* background_ = nullptr;  // wall and stand behind NABO, built once per lease
    bool background_ready_ = false;
    uint8_t* encoded_ = nullptr;
    size_t encoded_bytes_ = 0;
    TaskHandle_t task_ = nullptr;
    nabo_sd::Pack pack_;
    int want_action_ = 0;
    bool want_active_ = false;
    uint64_t want_start_ = 0;
    uint32_t seq_ = 0;
    std::atomic<uint32_t> epoch_{1};
    std::atomic<Status> status_{Scanning};
    uint32_t durations_[nabo_radio::kActionCount] = {};
    RadioFrame props_[kProps];
    std::atomic<bool> props_ready_{false};
    std::atomic<bool> rescan_{false};
};

namespace {

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    auto* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// Acoustic-foam dots, 18 px apart. Drawn as small squares and only inside the
// area being refreshed: NABO's frames repaint the wall behind him 8-16 times a
// second, and a tiled image on a rounded panel cost ~400 ms per refresh.
constexpr int kDotPitch = 18;

void DrawFoam(lv_event_t* event) {
    auto* layer = lv_event_get_layer(event);
    auto* wall = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    if (!layer || !wall)
        return;
    lv_area_t box, area;
    lv_obj_get_coords(wall, &box);
    box.y2 -= 40;  // the desk covers the bottom of the panel
    if (!lv_area_intersect(&area, &box, &layer->_clip_area))
        return;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_hex(kStageDot);
    const int first_x = box.x1 + ((area.x1 - box.x1 + 1) / kDotPitch) * kDotPitch + 7;
    const int first_y = box.y1 + ((area.y1 - box.y1 + 1) / kDotPitch) * kDotPitch + 7;
    for (int y = first_y - kDotPitch; y <= area.y2; y += kDotPitch)
        for (int x = first_x - kDotPitch; x <= area.x2; x += kDotPitch) {
            lv_area_t square{x, y, x + 3, y + 3};
            lv_area_t visible;
            if (lv_area_intersect(&visible, &square, &area))
                lv_draw_rect(layer, &dot, &square);
        }
}

}  // namespace

Tab5RadioStage::Tab5RadioStage(lv_obj_t* parent, int x, int y) {
    static_assert(kWidth == kStageW && kDeskY == kDeskTop, "reader and UI share the stage");
    static_assert(kNaboY + kCanvasH == kDeskTop, "NABO stands on the desk");
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_pos(root_, x, y);
    lv_obj_set_size(root_, kWidth, kHeight);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);

    // Wall: foam dots drawn per refresh (no bitmap) and three panel seams.
    wall_ = Box(root_, 0, 0, kWidth, kDeskY + 40, kStage, 26);
    lv_obj_add_event_cb(wall_, DrawFoam, LV_EVENT_DRAW_MAIN_END, nullptr);
    for (int seam = 140; seam < kWidth; seam += 142)
        Box(root_, seam, 0, 2, kDeskY, kStageLine);

    sign_ = lv_label_create(root_);
    lv_label_set_text(sign_, "ON AIR");
    lv_obj_set_pos(sign_, 28, 26);
    lv_obj_set_style_text_font(sign_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(sign_, 3, 0);
    lv_obj_set_style_pad_hor(sign_, 14, 0);
    lv_obj_set_style_pad_ver(sign_, 8, 0);
    lv_obj_set_style_radius(sign_, 8, 0);
    lv_obj_set_style_border_width(sign_, 2, 0);
    lv_obj_set_style_bg_color(sign_, lv_color_hex(kRed), 0);
    on_air_ = true;  // forces the first Tick to draw the unlit sign

    // Desk: a rounded bottom half under a square top half, so the wall meets it flush.
    Box(root_, 0, kDeskY + 40, kWidth, kHeight - kDeskY - 40, kDesk, 26);
    auto* desk = Box(root_, 0, kDeskY, kWidth, 60, kDesk);
    lv_obj_set_style_border_side(desk, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(desk, 4, 0);
    lv_obj_set_style_border_color(desk, lv_color_hex(kDeskHi), 0);

    // Stand, mic and mug sit below NABO's image. Where they overlap it, the reader
    // draws them into each frame, so nothing in LVGL covers NABO and his frames can
    // go straight to the panel.
    auto* arm = lv_line_create(root_);
    static lv_point_precise_t arm_points[] = {{kArmX0, kArmY0}, {kArmX1, kArmY1}};
    lv_line_set_points(arm, arm_points, 2);
    lv_obj_set_style_line_width(arm, kArmWidth, 0);
    lv_obj_set_style_line_color(arm, lv_color_hex(kMicArm), 0);
    lv_obj_set_style_line_rounded(arm, true, 0);
    mic_ = lv_image_create(root_);
    lv_obj_add_flag(mic_, LV_OBJ_FLAG_HIDDEN);
    mug_ = lv_image_create(root_);
    lv_obj_add_flag(mug_, LV_OBJ_FLAG_HIDDEN);

    nabo_ = lv_image_create(root_);
    lv_obj_set_pos(nabo_, kNaboX, kNaboY);
    lv_obj_add_flag(nabo_, LV_OBJ_FLAG_HIDDEN);

    // Coloured level bars, blue through violet to warm, centred on the desk. One
    // object draws all of them: 26 objects moving at once marked 50+ dirty areas,
    // overflowing LVGL's list and forcing a 250 ms full-screen redraw each time.
    bars_ = lv_obj_create(root_);
    lv_obj_remove_style_all(bars_);
    lv_obj_set_pos(bars_, kBarsX, kBarsMid - kBarMaxH / 2);
    lv_obj_set_size(bars_, kBars * (kBarW + kBarGap) - kBarGap, kBarMaxH);
    lv_obj_remove_flag(bars_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bars_, DrawBars, LV_EVENT_DRAW_MAIN, this);
    for (int i = 0; i < kBars; ++i)
        bar_heights_[i] = 4;

    knob_ = Box(root_, kWidth - 26 - 62, kDeskY + 26, 62, 62, kDialBg, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(knob_, 3, 0);
    lv_obj_set_style_border_color(knob_, lv_color_hex(kAmber), 0);
    lv_obj_set_style_transform_pivot_x(knob_, 31, 0);
    lv_obj_set_style_transform_pivot_y(knob_, 31, 0);
    Box(knob_, 26, 4, 4, 18, kAmber, 2);

    hint_ = lv_label_create(root_);
    lv_obj_set_width(hint_, kWidth - 80);
    lv_obj_set_pos(hint_, 40, 230);
    lv_obj_set_style_text_font(hint_, &qd_font_cjk_28, 0);
    lv_obj_set_style_text_color(hint_, lv_color_hex(kMuted), 0);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint_, "");

    lv_display_add_event_cb(lv_display_get_default(), RenderReady, LV_EVENT_RENDER_READY, this);
}

void Tab5RadioStage::Attach(lv_obj_t* parent, int x, int y) {
    if (lv_obj_get_parent(root_) != parent)
        lv_obj_set_parent(root_, parent);
    lv_obj_set_pos(root_, x, y);
}

void Tab5RadioStage::SetActive(bool active) {
    if (active && !reader_)
        reader_ = Tab5RadioStageReader::Get();
    active_ = active;
    if (!active) {
        // The frame buffers go back to the home scene; never draw from them again.
        lv_obj_add_flag(nabo_, LV_OBJ_FLAG_HIDDEN);
        direct_pending_ = false;
        action_ = nabo_radio::Action::Count;  // reopen restarts the current action
    }
    if (reader_)
        reader_->Want(
            static_cast<int>(action_ == nabo_radio::Action::Count ? nabo_radio::Action::IdleOff
                                                                  : action_),
            active);
}

void Tab5RadioStage::SetKnob(float position) {
    if (knob_)
        lv_obj_set_style_transform_rotation(knob_, int(std::lround(position * 240)) % 3600, 0);
}

void Tab5RadioStage::Tick(uint32_t now, const nabo_radio::Mood& mood, int level) {
    if (!active_)
        return;
    if (!lengths_set_ && reader_ && reader_->GetStatus() == Tab5RadioStageReader::Ready) {
        for (int i = 0; i < nabo_radio::kActionCount; ++i)
            director_.SetShotLength(static_cast<nabo_radio::Action>(i), reader_->DurationMs(i));
        lengths_set_ = true;
    }
    const nabo_radio::Action action = director_.Update(mood, now);
    const bool replay = director_.ShotSerial() != shot_serial_;
    shot_serial_ = director_.ShotSerial();
    if (action != action_ || (replay && nabo_radio::IsOneShot(action))) {
        action_ = action;
        if (reader_)
            reader_->Want(static_cast<int>(action), true, true);
    }
    PresentFrames();
    const bool live = mood.link == nabo_radio::Link::Playing;
    UpdateBars(live, live ? level : 0);
    if (live != on_air_) {
        on_air_ = live;
        lv_obj_set_style_bg_opa(sign_, live ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(sign_, lv_color_hex(live ? kRed : kRedOff), 0);
        lv_obj_set_style_text_color(sign_, lv_color_hex(live ? 0xfff3f1 : kRedOff), 0);
    }
    UpdateProps();
    UpdateHint();
}

void Tab5RadioStage::PresentFrames() {
    RadioFrame frame;
    if (!reader_)
        return;
    if (!reader_->TakeReady(&frame)) {
        if (direct_pending_)
            PresentDirect();
        return;
    }
    // LVGL renders synchronously on this task, so swapping the data pointer here
    // can never race a render that reads the previous buffer.
    FillDsc(&frame_dsc_, frame.pixels, frame.w, frame.h, true);
    lv_image_cache_drop(&frame_dsc_);
    if (lv_obj_has_flag(nabo_, LV_OBJ_FLAG_HIDDEN) || lv_image_get_src(nabo_) != &frame_dsc_) {
        // Entering goes through LVGL once.
        lv_image_set_src(nabo_, &frame_dsc_);
        lv_obj_remove_flag(nabo_, LV_OBJ_FLAG_HIDDEN);
        ++lvgl_frames_;
        return;
    }
    direct_tries_ = 0;
    PresentDirect();
    const uint32_t now = lv_tick_get();
    if (now - frames_reported_ms_ >= 10000) {
        ESP_LOGI(TAG, "frames direct=%u lvgl=%u", unsigned(direct_frames_), unsigned(lvgl_frames_));
        frames_reported_ms_ = now;
        direct_frames_ = lvgl_frames_ = 0;
    }
}

// Writes the current frame to the panel. If the panel is busy with an LVGL
// flush, the frame waits for the next tick: a full LVGL redraw of NABO costs
// ~250 ms, far more than waiting 50 ms. Only repeated failures fall back to it.
void Tab5RadioStage::PresentDirect() {
    lv_area_t area;
    lv_obj_get_coords(nabo_, &area);
    const auto* pixels = static_cast<const uint8_t*>(frame_dsc_.data);
    if (blit_ && DirectSafe(area) && blit_(blit_context_, area, pixels)) {
        // On the panel already, and LVGL will only ever read the new buffer.
        reader_->ReleaseRetired();
        direct_pending_ = false;
        ++direct_frames_;
        return;
    }
    if (blit_ && ++direct_tries_ < 4) {
        direct_pending_ = true;
        return;
    }
    direct_pending_ = false;
    lv_obj_invalidate(nabo_);
    ++lvgl_frames_;
}

// True when nothing drawn by LVGL lies over NABO's area, so a direct panel
// write cannot be overdrawn by, or overwrite, another object.
bool Tab5RadioStage::DirectSafe(const lv_area_t& area) const {
    if (lv_obj_get_screen(nabo_) != lv_screen_active() || !lv_obj_is_visible(nabo_) ||
        lv_obj_get_style_opa_recursive(nabo_, LV_PART_MAIN) < LV_OPA_COVER)
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
    for (lv_obj_t* obj = nabo_; lv_obj_get_parent(obj); obj = lv_obj_get_parent(obj)) {
        lv_obj_t* parent = lv_obj_get_parent(obj);
        lv_area_t bounds;
        lv_obj_get_coords(parent, &bounds);
        if (!lv_area_is_in(&area, &bounds, 0) || overlaps(parent, lv_obj_get_index(obj) + 1))
            return false;
    }
    return !overlaps(lv_layer_top(), 0) && !overlaps(lv_layer_sys(), 0);
}

void Tab5RadioStage::UpdateBars(bool live, int level) {
    // Called every 50 ms; one object redraws all bars at about 7 Hz.
    if (++bar_phase_ % 3 != 0)
        return;
    level = std::clamp(level, 0, 100);
    bool changed = live != bars_live_;
    bars_live_ = live;
    for (int i = 0; i < kBars; ++i) {
        const int ripple = 28 - std::abs((i * 13 + int(bar_phase_) * 5) % 56 - 28);
        const int target = live ? std::clamp(8 + level * (20 + ripple) / 45, 6, kBarMaxH) : 4;
        const int next = (bar_heights_[i] * 2 + target + 1) / 3;
        changed |= next != bar_heights_[i];
        bar_heights_[i] = next;
    }
    if (changed)
        lv_obj_invalidate(bars_);
}

void Tab5RadioStage::DrawBars(lv_event_t* event) {
    auto* self = static_cast<Tab5RadioStage*>(lv_event_get_user_data(event));
    auto* layer = lv_event_get_layer(event);
    if (!self || !layer)
        return;
    lv_area_t box;
    lv_obj_get_coords(self->bars_, &box);
    const int mid = (box.y1 + box.y2) / 2;
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = kBarW / 2;
    bar.bg_opa = self->bars_live_ ? LV_OPA_COVER : LV_OPA_40;
    for (int i = 0; i < kBars; ++i) {
        const int x = box.x1 + i * (kBarW + kBarGap), h = self->bar_heights_[i];
        bar.bg_color = lv_color_hsv_to_rgb((200 + i * 300 / kBars) % 360, 75, 95);
        lv_area_t area{x, mid - h / 2, x + kBarW - 1, mid - h / 2 + h - 1};
        lv_draw_rect(layer, &bar, &area);
    }
}

void Tab5RadioStage::UpdateProps() {
    if (props_shown_ || !reader_)
        return;
    const auto status = reader_->GetStatus();
    if (status == Tab5RadioStageReader::Scanning || status == Tab5RadioStageReader::NoCard)
        return;
    RadioFrame mic, mug;
    const bool has_mic = reader_->Prop(0, &mic), has_mug = reader_->Prop(1, &mug);
    props_shown_ = true;
    static lv_image_dsc_t mic_dsc, mug_dsc;
    if (has_mic) {
        FillDsc(&mic_dsc, mic.pixels, mic.w, mic.h);
        lv_image_set_src(mic_, &mic_dsc);
        lv_obj_set_pos(mic_, MicX(mic.w), kMicY);
        lv_obj_remove_flag(mic_, LV_OBJ_FLAG_HIDDEN);
    }
    if (has_mug) {
        FillDsc(&mug_dsc, mug.pixels, mug.w, mug.h);
        lv_image_set_src(mug_, &mug_dsc);
        lv_obj_set_pos(mug_, kMugX, MugY(mug.h));
        lv_obj_remove_flag(mug_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Tab5RadioStage::UpdateHint() {
    const int state = reader_ ? int(reader_->GetStatus()) : -2;
    if (state == hint_state_)
        return;
    hint_state_ = state;
    const char* text = "";
    switch (state) {
        case -2:
        case Tab5RadioStageReader::NoMemory:
            text = "内存不足，Nabo 暂时不能上台";
            break;
        case Tab5RadioStageReader::NoCard:
            text = "插入 SD 卡后 Nabo 就会出现";
            break;
        case Tab5RadioStageReader::NoPacks:
            text = "SD 卡里还没有 Nabo 的电台动画\n/tab5/nabo/radio-v1";
            break;
        default:
            break;
    }
    lv_label_set_text(hint_, text);
    // An empty label would still count as an object over NABO for direct output.
    if (*text)
        lv_obj_remove_flag(hint_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);
}

void Tab5RadioStage::RenderReady(lv_event_t* event) {
    auto* self = static_cast<Tab5RadioStage*>(lv_event_get_user_data(event));
    if (self && self->reader_)
        self->reader_->ReleaseRetired();
}
