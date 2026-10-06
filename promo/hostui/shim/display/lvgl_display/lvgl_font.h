#pragma once
#include "lvgl.h"
class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual void SetFallback(const lv_font_t* fallback) = 0;
    virtual ~LvglFont() = default;
};
class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t* font) : font_(font) {}
    const lv_font_t* font() const override { return font_; }
    void SetFallback(const lv_font_t*) override {}
private:
    const lv_font_t* font_;
};
