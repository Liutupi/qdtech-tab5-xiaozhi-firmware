#pragma once
#include "display.h"
#include "esp_lcd_types.h"
#include "display/lvgl_display/lvgl_image.h"
class LvglDisplay : public Display {
protected:
    lv_display_t* display_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
};
class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* content_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_;
};
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t, esp_lcd_panel_handle_t, int w, int h, int, int, bool, bool, bool) {
        display_ = lv_display_get_default(); width_ = w; height_ = h; }
};
