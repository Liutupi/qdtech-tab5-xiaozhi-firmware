#pragma once
#include <memory>
#include <string>
#include "display.h"
#include "lvgl_font.h"
class LvglTheme : public Theme {
public:
    LvglTheme(const std::string& n) : Theme(n) {}
    std::shared_ptr<LvglFont> GetTextFont() const override { return text_font_; }
    std::shared_ptr<LvglFont> text_font_;
};
class LvglThemeManager {
public:
    static LvglThemeManager& GetInstance() { static LvglThemeManager m; return m; }
    LvglTheme* GetTheme(const std::string&) { return &dark_; }
    LvglTheme dark_{"dark"};
};
