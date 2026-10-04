#pragma once
#include "sdkconfig.h"
#ifdef CONFIG_QDTECH_TAB5_SD_SCENE
#include "lvgl.h"
#include "nabo_scene_mailbox.h"
#include "nabo_scene_policy.h"
#include "nabo_three_state.h"
#include "nabo_wake_matches.h"

class Tab5SdSceneReader;
// All methods are called while the existing display/LVGL lock is held.
class Tab5SdScene {
public:
    using Input = nabo_scene::SceneInput;
    explicit Tab5SdScene(lv_obj_t* parent);
    // Writes an opaque RGB565 frame for a logical area straight to the panel,
    // bypassing LVGL rendering. Called on the LVGL task; returns false to fall
    // back to a normal invalidation.
    using DirectBlit = bool (*)(void* context, const lv_area_t& area, const uint8_t* pixels);
    void SetDirectBlit(DirectBlit blit, void* context);
    bool Tick(uint64_t now, Input input);
    void RenderReady();
    void Detach();
    lv_obj_t* TouchTarget() const { return root_; }
    bool WasSleeping() const { return was_sleeping_; }

private:
    void Source(const lv_image_dsc_t* dsc);
    void Present(nabo_sd::SceneMailbox::Lease next, uint64_t now);
    bool DirectSafe(const lv_area_t& area) const;
    void Face(unsigned eyes, unsigned mouth, uint8_t opacity);
    uint16_t* CaptureBackground();
    lv_obj_t *root_ = nullptr, *head_ = nullptr, *body_ = nullptr, *eyes_ = nullptr,
             *mouth_ = nullptr, *video_ = nullptr;
    uint16_t* background_ = nullptr;  // singleton-lifetime static art for the compositor
    uint8_t* overlay_ = nullptr;       // front decorations baked into composed frames
    DirectBlit blit_ = nullptr;
    void* blit_context_ = nullptr;
    lv_image_dsc_t live_{};  // the video layer's only source; data swaps per frame
    int live_slot_ = -1;
    uint32_t direct_frames_ = 0, lvgl_frames_ = 0, direct_max_us_ = 0, direct_total_us_ = 0;
    uint64_t blit_reported_ms_ = 0;
    Tab5SdSceneReader* reader_ = nullptr;
    nabo_scene::Controller controller_{nabo_scene::kWakeMatches};
    tab5_home::Animation speech_face_;
    nabo_scene::WorkClock work_clock_;
    const lv_image_dsc_t* source_ = nullptr;
    uint8_t retired_ = 0;
    uint32_t epoch_ = 1, last_generation_ = 0;
    int last_clip_ = -1;
    uint64_t last_frame_ms_ = 0, request_since_ = 0;
    bool was_sleeping_ = false, detached_ = false;
};
#endif
