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
    bool Tick(uint64_t now, Input input);
    void RenderReady();
    void Detach();
    lv_obj_t* TouchTarget() const { return root_; }
    bool WasSleeping() const { return was_sleeping_; }

private:
    void Source(const lv_image_dsc_t* dsc, nabo_sd::SceneMailbox::Lease lease = {});
    void Face(unsigned eyes, unsigned mouth, uint8_t opacity);
    lv_obj_t *root_ = nullptr, *head_ = nullptr, *body_ = nullptr, *eyes_ = nullptr,
             *mouth_ = nullptr;
    Tab5SdSceneReader* reader_ = nullptr;
    nabo_scene::Controller controller_{nabo_scene::kWakeMatches};
    tab5_home::Animation speech_face_;
    nabo_scene::WorkClock work_clock_;
    lv_image_dsc_t descriptors_[2]{};
    nabo_sd::SceneMailbox::Lease current_{};
    const lv_image_dsc_t* source_ = nullptr;
    uint8_t retired_ = 0;
    uint32_t epoch_ = 1, last_generation_ = 0;
    int last_clip_ = -1;
    uint64_t last_frame_ms_ = 0, request_since_ = 0;
    bool was_sleeping_ = false, detached_ = false;
};
#endif
