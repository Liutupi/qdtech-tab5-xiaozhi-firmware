#pragma once

#include <cstdint>

#include "lvgl.h"
#include "nabo_radio_mood.h"

class Tab5RadioStageReader;

// NABO FM studio on the left of the radio page: wall, ON AIR sign, NABO at
// 451x395, microphone, desk, coloured level bars and knob. NABO's actions
// stream from /sdcard/tab5/nabo/radio-v1 on a worker task, into frame buffers
// borrowed from the hidden home scene.
// All methods are called on the LVGL task with the display lock held.
class Tab5RadioStage {
public:
    static constexpr int kWidth = 568, kHeight = 628;

    Tab5RadioStage(lv_obj_t* parent, int x, int y);
    // Writes an opaque RGB565 frame for a screen area straight to the panel; false
    // falls back to LVGL. Same contract as Tab5SdScene::DirectBlit.
    using DirectBlit = bool (*)(void* context, const lv_area_t& area, const uint8_t* pixels);
    void SetDirectBlit(DirectBlit blit, void* context) {
        blit_ = blit;
        blit_context_ = context;
    }
    // Only a visible stage reads the card. Hiding it stops decoding.
    void SetActive(bool active);
    void Tick(uint32_t now, const nabo_radio::Mood& mood, int level);
    void Open(uint32_t now) { director_.Open(now); }
    void Favorite(uint32_t now) { director_.Favorite(now); }
    // The knob turns with the dial; one station step is one notch.
    void SetKnob(float position);

private:
    static constexpr int kDeskY = 518;
    static constexpr int kBars = 26, kBarW = 9, kBarGap = 4, kBarsX = 108, kBarMaxH = 64;
    static constexpr int kBarsMid = kDeskY + 57;
    void PresentFrames();
    bool DirectSafe(const lv_area_t& area) const;
    void PresentDirect();
    void UpdateBars(bool live, int level);
    void UpdateProps();
    void UpdateHint();
    static void RenderReady(lv_event_t* event);
    static void DrawBars(lv_event_t* event);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* wall_ = nullptr;
    lv_obj_t* sign_ = nullptr;
    lv_obj_t* nabo_ = nullptr;
    lv_obj_t* mic_ = nullptr;
    lv_obj_t* mug_ = nullptr;
    lv_obj_t* knob_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_obj_t* bars_ = nullptr;
    int bar_heights_[kBars] = {};
    unsigned bar_phase_ = 0;
    bool bars_live_ = true;
    lv_image_dsc_t frame_dsc_{};
    DirectBlit blit_ = nullptr;
    void* blit_context_ = nullptr;
    uint32_t direct_frames_ = 0, lvgl_frames_ = 0, frames_reported_ms_ = 0;
    bool direct_pending_ = false;
    int direct_tries_ = 0;
    bool on_air_ = false;
    bool active_ = false;
    bool props_shown_ = false;
    bool lengths_set_ = false;
    uint32_t shot_serial_ = 0;
    int hint_state_ = -1;
    nabo_radio::Action action_ = nabo_radio::Action::Count;
    nabo_radio::Director director_;
    Tab5RadioStageReader* reader_ = nullptr;
};
