#include "tab5_ir_remote_page.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <nvs.h>
#include <nvs_flash.h>

#include "board.h"
#include "display.h"
#include "ir_service.h"
#include "tab5_native_apps.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_lxgw_36);

namespace {
// Tab5 Port A: GND / EXT5V / G53 / G54. IR receiver DATA is on G54
// (captured real NEC frames there; G53 stayed silent). IR LED TX uses G53.
constexpr int kTxGpio = 53;
constexpr int kRxGpio = 54;

constexpr int kSlotCount = 20;
}  // namespace

Tab5IrRemotePage::Tab5IrRemotePage(lv_obj_t* parent, std::function<void()> back)
    : back_(std::move(back)) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 1280, 720);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x0d1b2b), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    Build();
}

void Tab5IrRemotePage::Show() {
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    SetStatus("红外遥控 · 学习后可控制电视/空调");
}

void Tab5IrRemotePage::Hide() {
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5IrRemotePage::SetStatus(const char* text) {
    if (!status_ || !text)
        return;
    // Application::Schedule is NOT the LVGL thread. Touching labels without
    // the display lock races the LVGL task and panics the UI (screen flash).
    DisplayLockGuard lock(Board::GetInstance().GetDisplay());
    if (status_)
        lv_label_set_text(status_, text);
}

void Tab5IrRemotePage::OnButton(lv_event_t* e) {
    auto* btn = static_cast<Btn*>(lv_event_get_user_data(e));
    if (!btn || !btn->page)
        return;
    auto* self = btn->page;
    switch (btn->kind) {
        case 0:
            self->SendNec(btn->a, btn->b, btn->label);
            break;
        case 1:
            self->SendSlot(btn->a, btn->label);
            break;
        case 2:
            self->StartLearn(btn->a, btn->label);
            break;
        case 3:
            self->SwitchTab(btn->a != 0);
            break;
        case 4:
            if (self->back_)
                self->back_();
            break;
        default:
            break;
    }
}

void Tab5IrRemotePage::SendNec(int addr, int cmd, const std::string& label) {
    bool ok = IrService::GetInstance().SendNec(kTxGpio, (uint8_t)addr, (uint8_t)cmd);
    SetStatus(ok ? ("已发送：" + label).c_str() : "发送失败，检查红外发射接线");
}

void Tab5IrRemotePage::SendSlot(int slot, const std::string& label) {
    if (slot < 0 || slot >= kSlotCount) {
        SetStatus("无效槽位");
        return;
    }
    nvs_handle_t h;
    if (nvs_open("ir_slots", NVS_READONLY, &h) != ESP_OK) {
        SetStatus("未学习该按键，请先学习");
        return;
    }
    size_t len = 512;
    uint16_t buf[256];
    char key[16];
    snprintf(key, sizeof(key), "s%d", slot);
    esp_err_t err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK || len < 4) {
        SetStatus(("未学习：" + label + "，请先学习").c_str());
        return;
    }
    bool ok = IrService::GetInstance().SendRaw(kTxGpio, buf, len / sizeof(uint16_t));
    // Also emit on G54 (same Port A pair) in case the LED is wired there.
    if (!ok)
        ok = IrService::GetInstance().SendRaw(kRxGpio, buf, len / sizeof(uint16_t));
    else
        IrService::GetInstance().SendRaw(kRxGpio, buf, len / sizeof(uint16_t));
    SetStatus(ok ? ("已发送：" + label).c_str() : "发送失败，检查红外发射接线");
}

void Tab5IrRemotePage::StartLearn(int slot, const std::string& label) {
    if (slot < 0 || slot >= kSlotCount) {
        SetStatus("无效槽位");
        return;
    }
    if (learning_) {
        SetStatus("已有学习任务进行中，请稍候");
        return;
    }
    learning_ = true;
    learn_slot_ = slot;
    learn_label_ = label;
    SetStatus(("学习中：用原装遥控器对准 Tab5，按「" + label + "」…").c_str());

    struct LearnJob {
        Tab5IrRemotePage* page;
        int slot;
        std::string label;
    };
    auto* job = new LearnJob{this, slot, label};
    xTaskCreate(
        [](void* arg) {
            LearnJob* job = static_cast<LearnJob*>(arg);
            std::string result = IrService::GetInstance().LearnFrame(kRxGpio, 10000);
            auto finish = [&](const std::string& msg) {
                job->page->learning_ = false;
                job->page->SetStatus(msg.c_str());
                delete job;
                vTaskDelete(nullptr);
            };
            auto pos = result.find("\"timings_us\":[");
            if (pos == std::string::npos) {
                finish("学习失败：未收到红外信号，请对准接收头再试");
                return;
            }
            std::vector<uint16_t> us;
            size_t i = pos + 14;
            while (i < result.size() && us.size() < 256) {
                size_t j = i;
                while (j < result.size() &&
                       (isdigit((unsigned char)result[j]) || result[j] == '-'))
                    ++j;
                if (j == i)
                    break;
                us.push_back((uint16_t)atoi(result.substr(i, j - i).c_str()));
                i = j + 1;
            }
            if (us.empty()) {
                finish("学习失败：帧为空");
                return;
            }
            nvs_handle_t h;
            if (nvs_open("ir_slots", NVS_READWRITE, &h) != ESP_OK) {
                finish("NVS 打开失败");
                return;
            }
            char key[16];
            snprintf(key, sizeof(key), "s%d", job->slot);
            nvs_set_blob(h, key, us.data(), us.size() * sizeof(uint16_t));
            nvs_commit(h);
            nvs_close(h);
            finish("已学习：" + job->label + "（" + std::to_string(us.size()) + " 个时序）");
        },
        "ir_learn", 8192, job, 2, nullptr);
}

void Tab5IrRemotePage::SwitchTab(bool tv) {
    if (tv) {
        lv_obj_clear_flag(panel_tv_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_ac_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(panel_tv_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(panel_ac_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Tab5IrRemotePage::Build() {
    auto* title = lv_label_create(root_);
    lv_label_set_text(title, "红外遥控");
    lv_obj_set_style_text_font(title, &qd_font_lxgw_36, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf5f9fd), 0);
    lv_obj_set_pos(title, 52, 24);

    status_ = lv_label_create(root_);
    lv_label_set_text(status_, "选择设备后使用");
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(status_, &qd_font_lxgw_28, 0);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x76d8ed), 0);
    lv_obj_set_width(status_, 900);
    lv_obj_set_pos(status_, 52, 78);

    // Back
    btns_[btn_count_] = {this, 4, 0, 0, "返回"};
    auto mkbtn = [this](lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                        Btn proto) {
        if (btn_count_ >= kMaxBtns)
            return;
        btns_[btn_count_] = proto;
        btns_[btn_count_].label = text ? text : "";
        auto* b = lv_btn_create(parent);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x193b56), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2a5f82), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x3a7490), 0);
        auto* lab = lv_label_create(b);
        lv_label_set_text(lab, btns_[btn_count_].label.c_str());
        lv_obj_set_style_text_font(lab, &qd_font_lxgw_28, 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(0xe1eff6), 0);
        lv_obj_center(lab);
        lv_obj_add_event_cb(b, OnButton, LV_EVENT_CLICKED, &btns_[btn_count_]);
        ++btn_count_;
    };

    mkbtn(root_, "返回", 1016, 24, 216, 60, {this, 4, 0, 0, "返回"});
    mkbtn(root_, "电视", 52, 130, 180, 56, {this, 3, 1, 0, "电视"});
    mkbtn(root_, "空调", 250, 130, 180, 56, {this, 3, 0, 0, "空调"});

    panel_tv_ = lv_obj_create(root_);
    lv_obj_set_pos(panel_tv_, 52, 200);
    lv_obj_set_size(panel_tv_, 1176, 480);
    lv_obj_set_style_bg_color(panel_tv_, lv_color_hex(0x122b43), 0);
    lv_obj_set_style_bg_opa(panel_tv_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel_tv_, 0, 0);
    lv_obj_set_style_radius(panel_tv_, 24, 0);
    lv_obj_set_style_pad_all(panel_tv_, 16, 0);
    lv_obj_clear_flag(panel_tv_, LV_OBJ_FLAG_SCROLLABLE);

    panel_ac_ = lv_obj_create(root_);
    lv_obj_set_pos(panel_ac_, 52, 200);
    lv_obj_set_size(panel_ac_, 1176, 480);
    lv_obj_set_style_bg_color(panel_ac_, lv_color_hex(0x122b43), 0);
    lv_obj_set_style_bg_opa(panel_ac_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel_ac_, 0, 0);
    lv_obj_set_style_radius(panel_ac_, 24, 0);
    lv_obj_set_style_pad_all(panel_ac_, 16, 0);
    lv_obj_clear_flag(panel_ac_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel_ac_, LV_OBJ_FLAG_HIDDEN);

    // TV page: power, vol, ch, dpad
    struct TvBtn {
        const char* t;
        int slot;
        int x, y;
    };
    // Layout 6 cols x 4 rows inside panel
    const TvBtn tv[] = {
        {"电源", 0, 20, 20},    {"主页", 11, 220, 20},   {"返回", 10, 420, 20},
        {"静音", 1, 620, 20},   {"音量+", 1, 820, 20},   {"音量-", 2, 1020, 20},
        {"上", 6, 320, 120},    {"OK", 5, 520, 120},     {"下", 7, 720, 120},
        {"左", 8, 120, 220},    {"右", 9, 920, 220},     {"频道+", 3, 20, 320},
        {"频道-", 4, 220, 320},
    };
    for (const auto& b : tv) {
        // send = learned slot; learn = same slot
        mkbtn(panel_tv_, b.t, b.x, b.y, 160, 70, {this, 1, b.slot, 0, b.t});
    }
    // learn buttons strip
    for (int i = 0; i < 6; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "学习%d", i + 1);
        mkbtn(panel_tv_, name, 20 + i * 190, 410, 170, 56,
              {this, 2, i, 0, name});
    }

    // AC page
    const TvBtn ac[] = {
        {"电源", 12, 20, 20},   {"模式", 15, 220, 20},   {"风量", 16, 420, 20},
        {"摆风", 17, 620, 20},  {"睡眠", 18, 820, 20},   {"灯光", 19, 1020, 20},
        {"温度+", 13, 320, 140}, {"温度-", 14, 720, 140},
    };
    for (const auto& b : ac) {
        mkbtn(panel_ac_, b.t, b.x, b.y, 160, 70, {this, 1, b.slot, 0, b.t});
    }
    for (int i = 0; i < 8; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "学习%d", i + 1);
        mkbtn(panel_ac_, name, 20 + i * 145, 280, 130, 56,
              {this, 2, 12 + i, 0, name});
    }

    SetStatus("先按「学习」再对准原装遥控器按键，即可保存该键码");
}
