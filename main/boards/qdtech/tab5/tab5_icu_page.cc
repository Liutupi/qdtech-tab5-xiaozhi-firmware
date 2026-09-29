#include "tab5_icu_page.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "icu_calculators.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_lxgw_36);

namespace {
constexpr const char* kModeNames[] = {"肾功能 eGFR", "尿白蛋白 uACR", "氧合", "血气分析", "静脉泵"};
constexpr const char* kFieldNames[][6] = {
    {"年龄 岁", "性别", "血肌酐 μmol/L", nullptr, nullptr, nullptr},
    {"尿白蛋白 mg/L", "尿肌酐 mmol/L", nullptr, nullptr, nullptr, nullptr},
    {"FiO₂ %", "PaO₂ mmHg", "平均气道压 cmH₂O", nullptr, nullptr, nullptr},
    {"pH", "PaCO₂ mmHg", "HCO₃⁻ mmol/L", "Na⁺ mmol/L", "Cl⁻ mmol/L", "白蛋白 g/L"},
    {"药物", "总药量", "最终总液量 mL", "泵速 mL/h", "体重 kg", nullptr},
};
constexpr const char* kKeys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "删除", "清空", "完成"};
}  // namespace

lv_obj_t* Tab5IcuPage::Panel(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    auto* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x31536c), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 22, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t* Tab5IcuPage::Label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                              int x, int y, int w, uint32_t color) {
    auto* obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t* Tab5IcuPage::Button(lv_obj_t* parent, const char* text,
                               int x, int y, int w, int h, lv_event_cb_t callback, void* user) {
    auto* obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x22465e), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x39718b), LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user);
    auto* label = Label(obj, text, &qd_font_lxgw_28, 12, 9, w - 24, 0xf3f8fb);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return obj;
}

Tab5IcuPage::Tab5IcuPage(lv_obj_t* parent, std::function<void()> on_back)
    : on_back_(std::move(on_back)) {
    page_ = Panel(parent, 0, 0, 1280, 720, 0x0d1b2b);
    lv_obj_set_style_radius(page_, 0, 0);
    lv_obj_set_style_border_width(page_, 0, 0);
    title_ = Label(page_, "ICU 数值工具", &qd_font_lxgw_36, 48, 24, 670, 0xf5f9fd);
    Label(page_, "输入实测值 · 核对单位 · 结果仅供复核", &qd_font_lxgw_28,
          50, 75, 760, 0x9cb9ca);
    Button(page_, "返回应用", 1010, 29, 220, 60, [](lv_event_t* e) {
        auto* self = static_cast<Tab5IcuPage*>(lv_event_get_user_data(e));
        self->Clear();
        if (self->on_back_) self->on_back_();
    }, this);

    auto* nav = Panel(page_, 42, 134, 224, 536, 0x142d43);
    for (int i = 0; i < kModes; ++i)
        nav_buttons_[i] = Button(nav, kModeNames[i], 16, 18 + 102 * i, 192, 78,
                                 OnNavigation, this);

    auto* input = Panel(page_, 282, 134, 462, 536, 0x142d43);
    Label(input, "输入", &qd_font_lxgw_36, 24, 14, 300, 0xf4f9fc);
    for (int i = 0; i < kFields; ++i) {
        field_buttons_[i] = Button(input, "", 22, 68 + 67 * i, 418, 59, OnField, this);
        field_labels_[i] = lv_obj_get_child(field_buttons_[i], 0);
        lv_obj_set_style_text_align(field_labels_[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(field_labels_[i], 14, 10);
        lv_obj_set_width(field_labels_[i], 390);
    }
    auto* calculate = Button(input, "计算", 22, 476, 418, 49, OnCalculate, this);
    lv_obj_set_style_bg_color(calculate, lv_color_hex(0x167f8d), 0);

    auto* result_panel = Panel(page_, 760, 134, 478, 536, 0x142d43);
    Label(result_panel, "换算结果", &qd_font_lxgw_36, 26, 18, 400, 0xf4f9fc);
    result_ = Label(result_panel, "填写左侧数值后点击计算。", &qd_font_lxgw_28,
                    27, 89, 420, 0xdbeaf3);
    lv_obj_set_height(result_, LV_SIZE_CONTENT);
    lv_obj_add_flag(result_panel, LV_OBJ_FLAG_SCROLLABLE);

    keypad_ = Panel(page_, 358, 93, 565, 533, 0x1b3a51);
    lv_obj_set_style_border_color(keypad_, lv_color_hex(0x86cddd), 0);
    keypad_value_ = Label(keypad_, "", &qd_font_lxgw_36, 25, 17, 510, 0xf8fcff);
    for (int i = 0; i < 12; ++i)
        keys_[i] = Button(keypad_, kKeys[i], 24 + (i % 3) * 173,
                          74 + (i / 3) * 83, 157, 68, OnKey, this);
    keys_[12] = Button(keypad_, kKeys[12], 24, 410, 245, 68, OnKey, this);
    keys_[13] = Button(keypad_, kKeys[13], 286, 410, 245, 68, OnKey, this);
    lv_obj_add_flag(keypad_, LV_OBJ_FLAG_HIDDEN);
    values_[4][2] = "50";
    SelectMode(0);
}

void Tab5IcuPage::Open(int mode, const std::string& external_result) {
    lv_obj_add_flag(keypad_, LV_OBJ_FLAG_HIDDEN);
    if (mode >= 0 && mode < kModes) SelectMode(mode);
    if (!external_result.empty()) lv_label_set_text(result_, external_result.c_str());
    lv_obj_remove_flag(page_, LV_OBJ_FLAG_HIDDEN);
}

void Tab5IcuPage::Clear() {
    for (auto& mode : values_) for (auto& value : mode) value.clear();
    values_[4][2] = "50";
    lv_label_set_text(result_, "填写左侧数值后点击计算。");
    lv_obj_set_style_text_color(result_, lv_color_hex(0xdbeaf3), 0);
    lv_obj_add_flag(keypad_, LV_OBJ_FLAG_HIDDEN);
    RefreshFields();
}

void Tab5IcuPage::SelectMode(int mode) {
    mode_ = mode;
    for (int i = 0; i < kModes; ++i)
        lv_obj_set_style_bg_color(nav_buttons_[i],
            lv_color_hex(i == mode ? 0x167f8d : 0x22465e), 0);
    lv_label_set_text(title_, kModeNames[mode]);
    lv_label_set_text(result_, "填写左侧数值后点击计算。");
    lv_obj_set_style_text_color(result_, lv_color_hex(0xdbeaf3), 0);
    RefreshFields();
}

void Tab5IcuPage::RefreshFields() {
    for (int i = 0; i < kFields; ++i) {
        if (!kFieldNames[mode_][i]) {
            lv_obj_add_flag(field_buttons_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(field_buttons_[i], LV_OBJ_FLAG_HIDDEN);
        const char* value = values_[mode_][i].empty() ? "—" : values_[mode_][i].c_str();
        std::string shown = std::string(kFieldNames[mode_][i]) + "   " + value;
        if (mode_ == 0 && i == 1) shown = std::string("性别   ") + (female_ ? "女" : "男") + "（点击切换）";
        if (mode_ == 4 && i == 0) {
            const auto& info = icu::GetDrugInfo(static_cast<icu::Drug>(drug_));
            shown = std::string(info.name) + "（点击换药）";
        }
        if (mode_ == 4 && i == 1)
            shown = std::string("总药量 / ") + icu::GetDrugInfo(static_cast<icu::Drug>(drug_)).input_unit + "   " + value;
        lv_label_set_text(field_labels_[i], shown.c_str());
    }
}

void Tab5IcuPage::OnNavigation(lv_event_t* event) {
    auto* self = static_cast<Tab5IcuPage*>(lv_event_get_user_data(event));
    if (!lv_obj_has_flag(self->keypad_, LV_OBJ_FLAG_HIDDEN)) return;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    for (int i = 0; i < kModes; ++i)
        if (target == self->nav_buttons_[i]) { self->SelectMode(i); return; }
}

void Tab5IcuPage::OnField(lv_event_t* event) {
    auto* self = static_cast<Tab5IcuPage*>(lv_event_get_user_data(event));
    if (!lv_obj_has_flag(self->keypad_, LV_OBJ_FLAG_HIDDEN)) return;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    for (int i = 0; i < kFields; ++i) {
        if (target != self->field_buttons_[i]) continue;
        if (self->mode_ == 0 && i == 1) { self->female_ = !self->female_; self->RefreshFields(); return; }
        if (self->mode_ == 4 && i == 0) {
            self->drug_ = (self->drug_ + 1) % icu::kDrugCount;
            self->values_[4][1].clear();
            self->RefreshFields();
            return;
        }
        self->editing_ = i;
        self->edit_value_ = self->values_[self->mode_][i];
        lv_label_set_text(self->keypad_value_, self->edit_value_.empty() ? "输入数值" : self->edit_value_.c_str());
        lv_obj_remove_flag(self->keypad_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(self->keypad_);
        return;
    }
}

void Tab5IcuPage::OnKey(lv_event_t* event) {
    auto* self = static_cast<Tab5IcuPage*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    for (int i = 0; i < 14; ++i) {
        if (target != self->keys_[i]) continue;
        if (i == 13) {
            self->values_[self->mode_][self->editing_] = self->edit_value_;
            lv_obj_add_flag(self->keypad_, LV_OBJ_FLAG_HIDDEN);
            self->RefreshFields();
        } else if (i == 12) self->edit_value_.clear();
        else if (i == 11) { if (!self->edit_value_.empty()) self->edit_value_.pop_back(); }
        else if (i == 9) {
            if (self->edit_value_.find('.') == std::string::npos)
                self->edit_value_ += self->edit_value_.empty() ? "0." : ".";
        } else if (self->edit_value_.size() < 9) self->edit_value_ += kKeys[i];
        if (i != 13) lv_label_set_text(self->keypad_value_,
            self->edit_value_.empty() ? "输入数值" : self->edit_value_.c_str());
        return;
    }
}

bool Tab5IcuPage::Number(int index, double& output, bool optional) const {
    const auto& value = values_[mode_][index];
    if (optional && value.empty()) { output = 0; return true; }
    return icu::ParseDecimal(value, output);
}

void Tab5IcuPage::Calculate() {
    double a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    icu::Result result{false, "请完整输入数值，并核对单位。"};
    switch (mode_) {
    case 0:
        if (Number(0, a) && Number(2, b)) result = icu::Egfr(a, female_, b);
        break;
    case 1:
        if (Number(0, a) && Number(1, b)) result = icu::Uacr(a, b);
        break;
    case 2:
        if (Number(0, a) && Number(1, b) && Number(2, c, true)) result = icu::Oxygen(a, b, c);
        break;
    case 3:
        if (Number(0, a) && Number(1, b) && Number(2, c) && Number(3, d, true) &&
            Number(4, e, true) && Number(5, f, true)) result = icu::BloodGas(a, b, c, d, e, f);
        break;
    case 4:
        if (Number(1, a) && Number(2, b) && Number(3, c) && Number(4, d, true))
            result = icu::Pump(static_cast<icu::Drug>(drug_), a, b, c, d);
        break;
    }
    lv_label_set_text(result_, result.text.c_str());
    lv_obj_set_style_text_color(result_, lv_color_hex(result.ok ? 0xdbeaf3 : 0xffbdad), 0);
}

void Tab5IcuPage::OnCalculate(lv_event_t* event) {
    auto* self = static_cast<Tab5IcuPage*>(lv_event_get_user_data(event));
    if (lv_obj_has_flag(self->keypad_, LV_OBJ_FLAG_HIDDEN)) self->Calculate();
}
