#include <cassert>
#include <string>

#include "tab5_icu_page.h"

extern const lv_font_t qd_font_lxgw_28{};
extern const lv_font_t qd_font_lxgw_36{};

namespace {
lv_obj_t* FindButton(lv_obj_t* root, const std::string& prefix) {
    if (root->callback && !root->children.empty() && root->children[0]->text.rfind(prefix, 0) == 0)
        return root;
    for (auto* child : root->children)
        if (auto* found = FindButton(child, prefix))
            return found;
    return nullptr;
}

lv_obj_t* FindLabel(lv_obj_t* root, const std::string& prefix) {
    if (root->text.rfind(prefix, 0) == 0)
        return root;
    for (auto* child : root->children)
        if (auto* found = FindLabel(child, prefix))
            return found;
    return nullptr;
}

void Click(lv_obj_t* button) {
    assert(button && button->callback);
    lv_event_t event{button, button->callback_user};
    button->callback(&event);
}

void Enter(lv_obj_t* root, const std::string& field, const std::string& digits) {
    Click(FindButton(root, field));
    Click(FindButton(root, "清空"));
    for (char digit : digits)
        Click(FindButton(root, std::string(1, digit)));
    Click(FindButton(root, "完成"));
}
}  // namespace

int main() {
    lv_obj_t root;
    Tab5IcuPage page(&root, [] {});
    auto* result = FindLabel(&root, "填写左侧数值后点击计算。");
    auto* voice_note = FindLabel(&root, "语音计算结果");
    auto* keypad_field = FindLabel(&root, "当前字段");
    assert(result && voice_note && keypad_field);

    Enter(&root, "年龄 岁", "60");
    assert(keypad_field->text == "年龄 岁");
    Enter(&root, "血肌酐", "88.4");
    Click(FindButton(&root, "计算"));
    assert(result->text.find("明确选择") != std::string::npos);
    Click(FindButton(&root, "性别"));  // First click explicitly chooses male.
    assert(result->text == "填写左侧数值后点击计算。");
    Click(FindButton(&root, "计算"));
    assert(result->text.find("eGFR  86.2") != std::string::npos);
    Click(FindButton(&root, "性别"));  // Female invalidates the old male result.
    assert(result->text == "填写左侧数值后点击计算。");
    Click(FindButton(&root, "计算"));
    assert(result->text.find("eGFR  64.5") != std::string::npos);
    Click(FindButton(&root, "年龄 岁"));
    Click(FindButton(&root, "删除"));
    assert(result->text == "填写左侧数值后点击计算。");
    Click(FindButton(&root, "完成"));

    page.Open(0, "语音范围错误", false);
    assert(result->text == "语音范围错误");
    assert(result->text_color == 0xffbdad);
    assert(!lv_obj_has_flag(voice_note, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(FindButton(&root, "年龄 岁"), LV_OBJ_FLAG_HIDDEN));
    Click(FindButton(&root, "手动输入"));
    assert(lv_obj_has_flag(voice_note, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(FindButton(&root, "年龄 岁"), LV_OBJ_FLAG_HIDDEN));
    assert(FindButton(&root, "年龄 岁")->children[0]->text.find("—") != std::string::npos);
    assert(FindButton(&root, "性别")->children[0]->text.find("—") != std::string::npos);

    page.Open(3);
    Click(FindButton(&root, "HCO₃⁻ mmol/L"));
    assert(keypad_field->text == "HCO₃⁻ mmol/L");
    Click(FindButton(&root, "完成"));

    page.Open(4);
    Click(FindButton(&root, "总药量"));
    assert(keypad_field->text == "总药量 / mg");
    Click(FindButton(&root, "完成"));
    Enter(&root, "总药量", "4");
    Enter(&root, "泵速", "3");
    Enter(&root, "体重", "80");
    Click(FindButton(&root, "计算"));
    assert(result->text.find("去甲肾上腺素  0.05") != std::string::npos);
    auto* drug_button = FindButton(&root, "去甲肾上腺素");
    Click(drug_button);
    assert(result->text == "填写左侧数值后点击计算。");
    for (int i = 0; i < 6; ++i)
        Click(drug_button);
    assert(drug_button->children[0]->text.rfind("血管加压素", 0) == 0);
    Click(FindButton(&root, "总药量"));
    assert(keypad_field->text == "总药量 / U");
    Click(FindButton(&root, "2"));
    Click(FindButton(&root, "完成"));
    assert(FindButton(&root, "总药量")->children[0]->text.find("2") != std::string::npos);
}
