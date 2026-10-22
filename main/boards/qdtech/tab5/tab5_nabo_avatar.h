#pragma once

#include <string>

#include "lvgl.h"
#include "nabo_scene_mailbox.h"

class Tab5NaboAvatarReader;

// A small, SD-backed portrait. All public methods belong to the LVGL task.
class Tab5NaboAvatar {
public:
    explicit Tab5NaboAvatar(lv_obj_t* parent);
    ~Tab5NaboAvatar();
    void SetMood(const std::string& mood);
    void Start();
    void Stop();

private:
    void Tick();
    static void RenderReady(lv_event_t* event);

    lv_obj_t* image_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    lv_display_t* display_ = nullptr;
    lv_image_dsc_t frame_{};
    Tab5NaboAvatarReader* reader_ = nullptr;
    nabo_sd::SceneMailbox::Lease shown_, retired_;
    uint32_t shown_frames_ = 0, reported_ms_ = 0;
    int clip_ = 0;
    bool active_ = false;
};
