#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"

// Full-screen IR remote page (TV + AC). Learn/send frames via IrService.
// Learned frames are stored in NVS under keys ir_slot_<n>.
class Tab5IrRemotePage {
public:
    explicit Tab5IrRemotePage(lv_obj_t* parent, std::function<void()> back);
    void Show();
    void Hide();

private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* tab_tv_ = nullptr;
    lv_obj_t* tab_ac_ = nullptr;
    lv_obj_t* panel_tv_ = nullptr;
    lv_obj_t* panel_ac_ = nullptr;
    std::function<void()> back_;
    std::atomic<bool> learning_{false};
    int learn_slot_ = -1;
    std::string learn_label_;

    void Build();
    void SetStatus(const char* text);
    void SwitchTab(bool tv);
    void SendNec(int addr, int cmd, const std::string& label);
    void SendSlot(int slot, const std::string& label);
    void StartLearn(int slot, const std::string& label);
    static void OnButton(lv_event_t* e);

    struct Btn {
        Tab5IrRemotePage* page;
        int kind;      // 0=nec, 1=slot, 2=learn, 3=tab, 4=back
        int a;         // addr or slot or tab
        int b;         // cmd
        std::string label;
    };
    static constexpr int kMaxBtns = 48;
    Btn btns_[kMaxBtns] = {};
    int btn_count_ = 0;
};
