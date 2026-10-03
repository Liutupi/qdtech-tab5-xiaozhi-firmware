#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct lv_font_t {};
#define LV_FONT_DECLARE(name) extern const lv_font_t name

struct lv_obj_t;
struct lv_event_t {
    lv_obj_t* target;
    void* user_data;
};
using lv_event_cb_t = void (*)(lv_event_t*);
using lv_color_t = uint32_t;

constexpr int LV_OBJ_FLAG_SCROLLABLE = 1;
constexpr int LV_OBJ_FLAG_HIDDEN = 2;
constexpr int LV_OPA_COVER = 255;
constexpr int LV_LABEL_LONG_WRAP = 1;
constexpr int LV_TEXT_ALIGN_CENTER = 1;
constexpr int LV_TEXT_ALIGN_LEFT = 2;
constexpr int LV_STATE_PRESSED = 1;
constexpr int LV_SIZE_CONTENT = -1;
constexpr int LV_EVENT_CLICKED = 1;

struct lv_obj_t {
    std::vector<lv_obj_t*> children;
    std::string text;
    uint32_t text_color = 0;
    int flags = 0;
    lv_event_cb_t callback = nullptr;
    void* callback_user = nullptr;

    ~lv_obj_t() {
        for (auto* child : children)
            delete child;
    }
};

inline lv_color_t lv_color_hex(uint32_t color) { return color; }
inline lv_obj_t* lv_obj_create(lv_obj_t* parent) {
    auto* obj = new lv_obj_t;
    parent->children.push_back(obj);
    return obj;
}
inline lv_obj_t* lv_label_create(lv_obj_t* parent) { return lv_obj_create(parent); }
inline lv_obj_t* lv_button_create(lv_obj_t* parent) { return lv_obj_create(parent); }
inline void lv_obj_set_pos(lv_obj_t*, int, int) {}
inline void lv_obj_set_size(lv_obj_t*, int, int) {}
inline void lv_obj_set_width(lv_obj_t*, int) {}
inline void lv_obj_set_height(lv_obj_t*, int) {}
inline void lv_obj_set_style_bg_color(lv_obj_t*, lv_color_t, int) {}
inline void lv_obj_set_style_bg_opa(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_border_color(lv_obj_t*, lv_color_t, int) {}
inline void lv_obj_set_style_border_width(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_radius(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_pad_all(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_shadow_width(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_text_align(lv_obj_t*, int, int) {}
inline void lv_obj_set_style_text_font(lv_obj_t*, const lv_font_t*, int) {}
inline void lv_obj_set_style_text_color(lv_obj_t* obj, lv_color_t color, int) {
    obj->text_color = color;
}
inline void lv_obj_clear_flag(lv_obj_t* obj, int flag) { obj->flags &= ~flag; }
inline void lv_obj_add_flag(lv_obj_t* obj, int flag) { obj->flags |= flag; }
inline void lv_obj_remove_flag(lv_obj_t* obj, int flag) { obj->flags &= ~flag; }
inline bool lv_obj_has_flag(const lv_obj_t* obj, int flag) { return (obj->flags & flag) != 0; }
inline void lv_obj_move_foreground(lv_obj_t*) {}
inline void lv_label_set_long_mode(lv_obj_t*, int) {}
inline void lv_label_set_text(lv_obj_t* obj, const char* text) { obj->text = text; }
inline void lv_obj_add_event_cb(lv_obj_t* obj, lv_event_cb_t callback, int, void* user) {
    obj->callback = callback;
    obj->callback_user = user;
}
inline lv_obj_t* lv_obj_get_child(lv_obj_t* obj, int index) {
    return index >= 0 && static_cast<size_t>(index) < obj->children.size() ? obj->children[index]
                                                                           : nullptr;
}
inline void* lv_event_get_user_data(lv_event_t* event) { return event->user_data; }
inline lv_obj_t* lv_event_get_target(lv_event_t* event) { return event->target; }
