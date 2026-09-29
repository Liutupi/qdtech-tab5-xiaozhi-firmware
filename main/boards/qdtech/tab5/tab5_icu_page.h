#pragma once

#include <array>
#include <functional>
#include <string>

#include "lvgl.h"

class Tab5IcuPage {
public:
    explicit Tab5IcuPage(lv_obj_t* parent, std::function<void()> on_back);
    lv_obj_t* object() const { return page_; }
    void Open(int mode = -1, const std::string& external_result = "");
    void Clear();

private:
    static constexpr int kModes = 5;
    static constexpr int kFields = 6;
    lv_obj_t* page_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* field_buttons_[kFields] = {};
    lv_obj_t* field_labels_[kFields] = {};
    lv_obj_t* nav_buttons_[kModes] = {};
    lv_obj_t* result_ = nullptr;
    lv_obj_t* keypad_ = nullptr;
    lv_obj_t* keypad_value_ = nullptr;
    lv_obj_t* keys_[14] = {};
    std::array<std::array<std::string, kFields>, kModes> values_{};
    std::function<void()> on_back_;
    int mode_ = 0;
    int editing_ = -1;
    int drug_ = 0;
    bool female_ = false;
    std::string edit_value_;

    static lv_obj_t* Panel(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color);
    static lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                           int x, int y, int w, uint32_t color);
    static lv_obj_t* Button(lv_obj_t* parent, const char* text,
                            int x, int y, int w, int h, lv_event_cb_t callback, void* user);
    static void OnNavigation(lv_event_t* event);
    static void OnField(lv_event_t* event);
    static void OnKey(lv_event_t* event);
    static void OnCalculate(lv_event_t* event);
    void SelectMode(int mode);
    void RefreshFields();
    void Calculate();
    bool Number(int index, double& output, bool optional = false) const;
};
