#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "lvgl.h"
#include "tab5_ui_motion.h"

LV_FONT_DECLARE(qd_font_cjk_28);

// Small LVGL builders and the colours of the 每日推送 page (dark ground, warm orange).
namespace tab5_nabo_ui {

constexpr uint32_t kBg = 0x0E1117, kCard = 0x171C25, kChip = 0x222833, kSelected = 0x1C222C,
                   kRow = 0x141820, kOrange = 0xF5A04A, kInk = 0x1A1206, kText = 0xF2EEE8,
                   kMuted = 0x8A93A3, kSoft = 0xAEB6C3, kBody = 0xE6E1D8, kFaint = 0x6F7888,
                   kGreen = 0x7BD8A6, kRose = 0xFF9DAF, kPink = 0xFF9DBB, kBlue = 0x8CC0FF,
                   kPurple = 0xC9B2FF, kNote = 0x1E1A14, kNoteText = 0xF5C08A;

inline lv_color_t Hex(uint32_t color) { return lv_color_hex(color); }

inline lv_obj_t* Panel(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, Hex(color), 0);
    lv_obj_set_style_bg_opa(obj, color == kBg ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// w > 0 wraps at that width; LV_SIZE_CONTENT keeps one line.
inline lv_obj_t* Label(lv_obj_t* parent, const std::string& text, const lv_font_t* font,
                       uint32_t color, int x, int y, int w) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, Hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    if (w > 0)
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

inline void Clickable(lv_obj_t* obj, lv_event_cb_t cb, void* user, int tag = 0) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(intptr_t(tag)));
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user);
    tab5_ui::AddPressFeel(obj);
}

inline int TagOf(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    return int(intptr_t(lv_obj_get_user_data(target)));
}

inline lv_obj_t* Button(lv_obj_t* parent, const std::string& text, const lv_font_t* font, int x,
                        int y, int w, int h, uint32_t bg, uint32_t fg, lv_event_cb_t cb, void* user,
                        int tag = 0) {
    auto* button = Panel(parent, x, y, w, h, bg, h / 2);
    if (bg == kBg)
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    Clickable(button, cb, user, tag);
    auto* label = Label(button, text, font, fg, 0, 0, LV_SIZE_CONTENT);
    lv_obj_center(label);
    return button;
}

// A rounded tag: tinted ground, coloured text.
inline lv_obj_t* Tag(lv_obj_t* parent, const std::string& text, uint32_t color, int x, int y,
                     const lv_font_t* font = nullptr) {
    auto* tag = Panel(parent, x, y, LV_SIZE_CONTENT, 40, color, 20);
    lv_obj_set_style_bg_opa(tag, LV_OPA_20, 0);
    lv_obj_set_style_pad_hor(tag, 16, 0);
    auto* label = Label(tag, text, font ? font : &qd_font_cjk_28, color, 0, 0, LV_SIZE_CONTENT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    return tag;
}

inline lv_obj_t* Column(lv_obj_t* parent, int x, int y, int w, int h) {
    auto* column = Panel(parent, x, y, w, h, kBg, 0);
    lv_obj_add_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(column, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(column, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 14, 0);
    lv_obj_set_style_pad_bottom(column, 16, 0);
    return column;
}

inline lv_obj_t* Row(lv_obj_t* parent, int w) {
    auto* row = Panel(parent, 0, 0, w, LV_SIZE_CONTENT, kBg, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_row(row, 10, 0);
    return row;
}

inline void SetHidden(lv_obj_t* obj, bool hidden) {
    if (hidden)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// Rebuilds run after the event that asked for them has finished.
inline void Defer(void* self, lv_async_cb_t cb) { lv_async_call(cb, self); }

// The device fonts have no star glyphs: text says "4/5", graphics use five dots.
inline std::string Score(int score) { return std::to_string(score) + "/5"; }

inline lv_obj_t* ScoreDots(lv_obj_t* parent, int score, int size) {
    auto* row = Panel(parent, 0, 0, 5 * size + 4 * (size / 2), size, kBg, 0);
    for (int i = 0; i < 5; ++i) {
        auto* dot = Panel(row, i * (size + size / 2), 0, size, size, i < score ? kOrange : kBg,
                          LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, Hex(kOrange), 0);
    }
    return row;
}

// "2026-10-22" → "周四" (Sakamoto's day-of-week).
inline std::string Weekday(const std::string& day) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(day.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12)
        return {};
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    y -= m < 3;
    static const char* const names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return names[(y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7];
}

// "2026-10-22" → "10月22日 周四".
inline std::string DayTitle(const std::string& day) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(day.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3)
        return "每日推送";
    return std::to_string(m) + "月" + std::to_string(d) + "日 " + Weekday(day);
}

}  // namespace tab5_nabo_ui
