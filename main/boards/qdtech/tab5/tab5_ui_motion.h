#pragma once

#include "lvgl.h"

// Small motion helpers for the native Tab5 UI. They only animate translate and
// transform_width/height: none of these makes LVGL render through an
// intermediate layer, so a press or a page change costs no extra RAM and only
// redraws what moves. Timing is absolute (lv_anim), so a slow frame skips ahead
// instead of stretching the motion.
namespace tab5_ui {

constexpr int kPageSlidePx = 72;
constexpr uint32_t kPageSlideMs = 240;

inline void SetTranslateX(void* obj, int32_t value) {
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), value, 0);
}

// Slides a page that was just made visible in from the right (forward) or the
// left (back). The root behind every page is the same solid colour, so the
// strip the page uncovers looks like part of the motion.
inline void SlideIn(lv_obj_t* page, bool forward) {
    if (!page)
        return;
    lv_anim_delete(page, SetTranslateX);
    const int from = forward ? kPageSlidePx : -kPageSlidePx;
    SetTranslateX(page, from);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, page);
    lv_anim_set_values(&anim, from, 0);
    lv_anim_set_duration(&anim, kPageSlideMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, SetTranslateX);
    lv_anim_start(&anim);
}

// Press feel for cards and buttons: on touch the surface sinks (moves down 3 px,
// shrinks 3 px per side) within 70 ms; on release it springs back with a slight
// overshoot. Shared static styles, so adding it to many objects costs nothing.
inline void AddPressFeel(lv_obj_t* obj) {
    static bool ready = false;
    static lv_style_t normal;
    static lv_style_t pressed;
    static lv_style_transition_dsc_t press_in;
    static lv_style_transition_dsc_t release;
    static const lv_style_prop_t kProps[] = {LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT,
                                             LV_STYLE_TRANSLATE_Y, LV_STYLE_PROP_INV};
    if (!ready) {
        ready = true;
        lv_style_transition_dsc_init(&press_in, kProps, lv_anim_path_ease_out, 70, 0, nullptr);
        lv_style_transition_dsc_init(&release, kProps, lv_anim_path_overshoot, 260, 0, nullptr);
        lv_style_init(&normal);
        lv_style_set_transition(&normal, &release);
        lv_style_init(&pressed);
        lv_style_set_transform_width(&pressed, -3);
        lv_style_set_transform_height(&pressed, -3);
        lv_style_set_translate_y(&pressed, 3);
        lv_style_set_transition(&pressed, &press_in);
    }
    if (!obj)
        return;
    lv_obj_add_style(obj, &normal, 0);
    lv_obj_add_style(obj, &pressed, LV_STATE_PRESSED);
}

}  // namespace tab5_ui
