#pragma once

#include "lvgl.h"

// Press feel for the native Tab5 UI. It only animates translate and
// transform_width/height: neither makes LVGL render through an intermediate
// layer, so a press costs no extra RAM and only redraws the pressed control.
// Page changes fade with the backlight instead (tab5_page_fade.h).
namespace tab5_ui {

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
