#include "tab5_ir_remote_page.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <algorithm>

#include "board.h"
#include "display.h"
#include "ir_service.h"
#include "ir_store.h"

LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_cjk_28);

namespace {
const char* TAG = "Tab5IrPage";

enum Kind {
    kBack = 0,
    kSelfTest,
    kSelectDevice,
    kAddDeviceOverlay,
    kLearnToggle,
    kAddKeyOverlay,
    kDeleteDevice,
    kKey,
    kChooseType,
    kChooseKeyName,
    kCloseOverlay,
};

constexpr uint32_t kBg = 0x0d1b2b;
constexpr uint32_t kPanel = 0x122b43;
constexpr uint32_t kLearned = 0x1d6f8a;
constexpr uint32_t kUnlearned = 0x1a2c3d;
constexpr uint32_t kAccent = 0xf0a33a;

lv_obj_t* Panel(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    auto* o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 22, 0);
    lv_obj_set_style_pad_all(o, 14, 0);
    return o;
}

lv_obj_t* Text(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
    auto* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void Flow(lv_obj_t* o, lv_flex_flow_t flow, int gap) {
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
}
}  // namespace

Tab5IrRemotePage::Tab5IrRemotePage(lv_obj_t* parent, std::function<void()> back) : back_(std::move(back)) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 1280, 720);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);

    jobs_ = xQueueCreate(4, sizeof(Job*));
    xTaskCreate(Worker, "ir_worker", 8192, this, 3, nullptr);
    ir::Store::GetInstance().Load();
    Build();
}

void Tab5IrRemotePage::Show() {
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    RefreshDevices();
    if (!checked_polarity_) {
        checked_polarity_ = true;
        if (!ir::Store::GetInstance().GetConfig().polarity_checked)
            SelfTest(false);  // quiet TX check on first use
    }
}

void Tab5IrRemotePage::Hide() {
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

// ---------------------------------------------------------------- helpers

Tab5IrRemotePage::Tag* Tab5IrRemotePage::NewTag(int kind, int a, const std::string& text, bool overlay) {
    auto* t = new Tag{this, kind, a, text};
    (overlay ? overlay_tags_ : tags_).push_back(t);
    return t;
}

lv_obj_t* Tab5IrRemotePage::MakeButton(lv_obj_t* parent, const std::string& text, int w, int h, uint32_t bg,
                                       uint32_t fg, Tag* tag) {
    auto* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_lighten(lv_color_hex(bg), LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x3a7490), 0);
    auto* l = Text(b, text.c_str(), &qd_font_cjk_28, fg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, LV_MIN(w - 16, 400));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (tag) {
        lv_obj_add_event_cb(b, OnEvent, LV_EVENT_SHORT_CLICKED, tag);
        if (tag->kind == kKey)
            lv_obj_add_event_cb(b, OnEvent, LV_EVENT_LONG_PRESSED, tag);
    }
    return b;
}

void Tab5IrRemotePage::SetStatusLocked(const std::string& text) {
    if (status_)
        lv_label_set_text(status_, text.c_str());
}

void Tab5IrRemotePage::SetStatus(const std::string& text) {
    // Called from the worker task: LVGL needs the display lock.
    DisplayLockGuard lock(Board::GetInstance().GetDisplay());
    SetStatusLocked(text);
}

// ---------------------------------------------------------------- layout

void Tab5IrRemotePage::Build() {
    auto* title = Text(root_, "红外遥控", &qd_font_lxgw_36, 0xf5f9fd);
    lv_obj_set_pos(title, 52, 24);

    status_ = Text(root_, "", &qd_font_cjk_28, 0x76d8ed);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(status_, 1176, 72);
    lv_obj_set_pos(status_, 52, 80);

    auto* test = MakeButton(root_, "自检", 180, 60, 0x254c66, 0xf1f8fc, NewTag(kSelfTest, 0, ""));
    lv_obj_set_pos(test, 816, 20);
    auto* back = MakeButton(root_, "返回", 200, 60, 0x254c66, 0xf1f8fc, NewTag(kBack, 0, ""));
    lv_obj_set_pos(back, 1028, 20);

    device_list_ = Panel(root_, 52, 156, 268, 548, kPanel);
    Flow(device_list_, LV_FLEX_FLOW_COLUMN, 10);
    lv_obj_set_scroll_dir(device_list_, LV_DIR_VER);

    auto* right = Panel(root_, 336, 156, 892, 548, kPanel);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    device_title_ = Text(right, "", &qd_font_cjk_28, 0xf5f9fd);
    lv_obj_set_pos(device_title_, 8, 14);
    lv_obj_set_width(device_title_, 300);
    lv_label_set_long_mode(device_title_, LV_LABEL_LONG_DOT);

    learn_btn_ = MakeButton(right, "学习模式", 170, 56, 0x254c66, 0xf1f8fc, NewTag(kLearnToggle, 0, ""));
    lv_obj_set_pos(learn_btn_, 320, 0);
    learn_label_ = lv_obj_get_child(learn_btn_, 0);
    auto* add = MakeButton(right, "添加按键", 170, 56, 0x254c66, 0xf1f8fc, NewTag(kAddKeyOverlay, 0, ""));
    lv_obj_set_pos(add, 502, 0);
    auto* del = MakeButton(right, "删除设备", 170, 56, 0x4a2630, 0xffd7d7, NewTag(kDeleteDevice, 0, ""));
    lv_obj_set_pos(del, 684, 0);
    delete_label_ = lv_obj_get_child(del, 0);

    key_grid_ = lv_obj_create(right);
    lv_obj_set_pos(key_grid_, 0, 70);
    lv_obj_set_size(key_grid_, 862, 448);
    lv_obj_set_style_bg_opa(key_grid_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(key_grid_, 0, 0);
    lv_obj_set_style_pad_all(key_grid_, 4, 0);
    Flow(key_grid_, LV_FLEX_FLOW_ROW_WRAP, 12);
    lv_obj_set_scroll_dir(key_grid_, LV_DIR_VER);
}

void Tab5IrRemotePage::RefreshDevices() {
    // Rebuilding deletes the buttons that may have fired the current event: callers come in
    // through lv_async_call, never from inside a button's own callback.
    auto devices = ir::Store::GetInstance().Devices();
    if (current_device_ < 0 || std::none_of(devices.begin(), devices.end(),
                                            [this](const ir::Device& d) { return d.id == current_device_; }))
        current_device_ = devices.empty() ? -1 : devices.front().id;

    lv_obj_clean(device_list_);
    DropTags({kSelectDevice, kAddDeviceOverlay});

    for (const auto& d : devices) {
        int learned = 0;
        for (const auto& k : d.keys)
            learned += k.learned();
        const bool sel = d.id == current_device_;
        auto* b = MakeButton(device_list_, d.name + "  " + std::to_string(learned), 236, 64,
                             sel ? kLearned : kUnlearned, 0xf1f8fc, NewTag(kSelectDevice, d.id, d.name));
        if (sel)
            lv_obj_set_style_border_color(b, lv_color_hex(0x8fe3f5), 0);
    }
    MakeButton(device_list_, "+ 添加设备", 236, 64, 0x2c3e2a, 0xd8f5c8, NewTag(kAddDeviceOverlay, 0, ""));
    RefreshKeys();
}

void Tab5IrRemotePage::DropTags(std::initializer_list<int> kinds) {
    tags_.erase(std::remove_if(tags_.begin(), tags_.end(),
                               [&kinds](Tag* t) {
                                   if (std::find(kinds.begin(), kinds.end(), t->kind) == kinds.end())
                                       return false;
                                   delete t;
                                   return true;
                               }),
                tags_.end());
}

void Tab5IrRemotePage::RefreshKeys() {
    lv_obj_clean(key_grid_);
    DropTags({kKey});
    lv_label_set_text(learn_label_, learn_mode_ ? "结束学习" : "学习模式");
    lv_obj_set_style_bg_color(learn_btn_, lv_color_hex(learn_mode_ ? kAccent : 0x254c66), 0);
    lv_label_set_text(delete_label_, "删除设备");
    ir::Device dev;
    if (current_device_ < 0 || !ir::Store::GetInstance().GetDevice(current_device_, &dev)) {
        lv_label_set_text(device_title_, "还没有设备");
        auto* hint = Text(key_grid_,
                          "点左侧《+ 添加设备》，选择电视、空调、风扇等类型；\n"
                          "再打开《学习模式》，点一个按键后用原装遥控器对准 Tab5 按一下即可。",
                          &qd_font_cjk_28, 0xa8c4d3);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, 840);
        return;
    }
    lv_label_set_text(device_title_, dev.name.c_str());
    for (const auto& k : dev.keys) {
        const bool learned = k.learned();
        const bool target = learn_mode_ && k.id == learn_target_;
        auto* b = MakeButton(key_grid_, k.name, 196, 76, target ? kAccent : (learned ? kLearned : kUnlearned),
                             target || learned ? 0xf1f8fc : 0x7f95a6, NewTag(kKey, k.id, k.name));
        if (learn_mode_)
            lv_obj_set_style_border_color(b, lv_color_hex(kAccent), 0);
        if (target)
            lv_obj_set_style_border_width(b, 4, 0);
    }
}

// ---------------------------------------------------------------- overlays

void Tab5IrRemotePage::OpenOverlay(Overlay kind) {
    CloseOverlay();
    overlay_ = lv_obj_create(root_);
    lv_obj_set_size(overlay_, 1280, 720);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_70, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    auto* box = Panel(overlay_, 140, 80, 1000, 560, 0x163550);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    auto* title = Text(box, kind == Overlay::kAddDevice ? "选择设备类型" : "选择要添加的按键",
                       &qd_font_cjk_28, 0xf5f9fd);
    lv_obj_set_pos(title, 10, 14);
    auto* close = MakeButton(box, "取消", 150, 56, 0x254c66, 0xf1f8fc, NewTag(kCloseOverlay, 0, "", true));
    lv_obj_set_pos(close, 810, 0);
    auto* grid = lv_obj_create(box);
    lv_obj_set_pos(grid, 0, 72);
    lv_obj_set_size(grid, 970, 460);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 4, 0);
    Flow(grid, LV_FLEX_FLOW_ROW_WRAP, 12);
    if (kind == Overlay::kAddDevice) {
        for (const auto& t : ir::DeviceTypes())
            MakeButton(grid, t.label, 220, 90, kLearned, 0xf1f8fc, NewTag(kChooseType, 0, t.type, true));
    } else {
        ir::Device dev;
        ir::Store::GetInstance().GetDevice(current_device_, &dev);
        for (const auto& name : ir::SuggestedKeys(dev.type)) {
            const bool exists = std::any_of(dev.keys.begin(), dev.keys.end(),
                                            [&name](const ir::Key& k) { return k.name == name; });
            if (!exists)
                MakeButton(grid, name, 172, 70, kUnlearned, 0xf1f8fc, NewTag(kChooseKeyName, 0, name, true));
        }
    }
}

void Tab5IrRemotePage::CloseOverlay() {
    if (overlay_) {
        lv_obj_delete(overlay_);
        overlay_ = nullptr;
    }
    for (auto* t : overlay_tags_)
        delete t;
    overlay_tags_.clear();
}

// ---------------------------------------------------------------- events

void Tab5IrRemotePage::OnEvent(lv_event_t* e) {
    auto* tag = static_cast<Tag*>(lv_event_get_user_data(e));
    if (!tag || !tag->page)
        return;
    auto* self = tag->page;
    const bool long_press = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    const int kind = tag->kind;
    const int a = tag->a;
    const std::string text = tag->text;
    // Anything that rebuilds widgets runs after this callback returns.
    auto later = [self](std::function<void(Tab5IrRemotePage*)> fn) {
        auto* call = new std::pair<Tab5IrRemotePage*, std::function<void(Tab5IrRemotePage*)>>(self, std::move(fn));
        lv_async_call(
            [](void* p) {
                auto* c = static_cast<std::pair<Tab5IrRemotePage*, std::function<void(Tab5IrRemotePage*)>>*>(p);
                c->second(c->first);
                delete c;
            },
            call);
    };
    switch (kind) {
        case kBack:
            later([](Tab5IrRemotePage* p) {
                p->CloseOverlay();
                p->StopLearning();
                if (p->back_)
                    p->back_();
            });
            break;
        case kSelfTest:
            self->SelfTest(true);
            break;
        case kSelectDevice:
            later([a](Tab5IrRemotePage* p) {
                p->StopLearning();
                p->current_device_ = a;
                p->RefreshDevices();
            });
            break;
        case kAddDeviceOverlay:
            later([](Tab5IrRemotePage* p) { p->OpenOverlay(Overlay::kAddDevice); });
            break;
        case kLearnToggle:
            later([](Tab5IrRemotePage* p) {
                if (p->learn_mode_) {
                    p->StopLearning();
                    p->RefreshKeys();
                    p->SetStatusLocked("已结束学习：点亮的按键可以直接发送");
                } else {
                    p->StartGuidedLearning();
                }
            });
            break;
        case kAddKeyOverlay:
            if (self->current_device_ < 0) {
                self->SetStatusLocked("请先添加设备");
                break;
            }
            later([](Tab5IrRemotePage* p) { p->OpenOverlay(Overlay::kAddKey); });
            break;
        case kDeleteDevice: {
            if (self->current_device_ < 0)
                break;
            const int64_t now = esp_timer_get_time();
            if (now - self->delete_armed_us_ > 4000000) {
                self->delete_armed_us_ = now;
                lv_label_set_text(self->delete_label_, "再点确认");
                self->SetStatusLocked("再点一次《删除设备》确认删除(已学的按键会一起删除)");
                break;
            }
            self->delete_armed_us_ = 0;
            later([](Tab5IrRemotePage* p) {
                ir::Store::GetInstance().RemoveDevice(p->current_device_);
                p->current_device_ = -1;
                p->RefreshDevices();
                p->SetStatusLocked("设备已删除");
            });
            break;
        }
        case kKey:
            self->OnKey(a, long_press);
            break;
        case kChooseType:
            later([text](Tab5IrRemotePage* p) {
                p->CloseOverlay();
                const int id = ir::Store::GetInstance().AddDevice(text);
                if (id < 0) {
                    p->SetStatusLocked("设备太多了，先删除不用的设备");
                    return;
                }
                p->current_device_ = id;
                p->RefreshDevices();
                p->StartGuidedLearning();
            });
            break;
        case kChooseKeyName:
            later([text](Tab5IrRemotePage* p) {
                p->CloseOverlay();
                const int key = ir::Store::GetInstance().AddKey(p->current_device_, text);
                p->learn_mode_ = true;
                p->RefreshDevices();
                if (key > 0)
                    p->Learn(key, text, false);
            });
            break;
        case kCloseOverlay:
            later([](Tab5IrRemotePage* p) { p->CloseOverlay(); });
            break;
        default:
            break;
    }
}

void Tab5IrRemotePage::OnKey(int key_id, bool long_press) {
    ir::Device dev;
    if (!ir::Store::GetInstance().GetDevice(current_device_, &dev))
        return;
    std::string name;
    bool learned = false;
    for (const auto& k : dev.keys)
        if (k.id == key_id) {
            name = k.name;
            learned = k.learned();
        }
    if (name.empty())
        return;
    if (learn_mode_ && long_press) {
        // Long press in learning mode deletes the key (press twice to confirm).
        const int64_t now = esp_timer_get_time();
        if (pending_key_delete_ != key_id || now - key_delete_armed_us_ > 4000000) {
            pending_key_delete_ = key_id;
            key_delete_armed_us_ = now;
            SetStatusLocked("再长按一次《" + name + "》删除这个按键");
            return;
        }
        pending_key_delete_ = -1;
        ir::Store::GetInstance().RemoveKey(current_device_, key_id);
        lv_async_call([](void* p) { static_cast<Tab5IrRemotePage*>(p)->RefreshDevices(); }, this);
        SetStatusLocked("已删除《" + name + "》");
        return;
    }
    if (learn_mode_ || long_press || !learned) {
        if (!learn_mode_ && !learned && !long_press) {
            SetStatusLocked("《" + name + "》还没学习：点《学习模式》(或直接长按这个键)");
            return;
        }
        if (!learn_mode_) {
            learn_mode_ = true;  // long press: learn just this key
            Learn(key_id, name, false);
        } else {
            Learn(key_id, name, true);
        }
        return;
    }
    Send(key_id, name);
}

// ---------------------------------------------------------------- worker jobs

void Tab5IrRemotePage::Post(std::function<void()> run) {
    auto* job = new Job{std::move(run)};
    if (xQueueSend(jobs_, &job, 0) != pdTRUE) {
        delete job;
        SetStatusLocked("操作太快了，请稍候再试");
    }
}

void Tab5IrRemotePage::StopLearning() {
    learn_mode_ = false;
    learn_target_ = -1;
    ++learn_gen_;
    cancel_learn_ = true;
}

void Tab5IrRemotePage::StartGuidedLearning() {
    ir::Device dev;
    if (!ir::Store::GetInstance().GetDevice(current_device_, &dev) || dev.keys.empty()) {
        SetStatusLocked("请先添加设备和按键");
        return;
    }
    learn_mode_ = true;
    const ir::Key* first = nullptr;
    for (const auto& k : dev.keys)
        if (!k.learned()) {
            first = &k;
            break;
        }
    if (!first) {
        learn_target_ = -1;
        RefreshKeys();
        SetStatusLocked("这个设备的键都学过了。点任意键可重新学习，或点《添加按键》");
        return;
    }
    Learn(first->id, first->name, true);
}

void Tab5IrRemotePage::Worker(void* arg) {
    auto* self = static_cast<Tab5IrRemotePage*>(arg);
    while (true) {
        Job* job = nullptr;
        if (xQueueReceive(self->jobs_, &job, portMAX_DELAY) == pdTRUE && job) {
            job->run();
            delete job;
        }
    }
}

void Tab5IrRemotePage::Learn(int key_id, const std::string& name, bool auto_advance) {
    const int device = current_device_;
    learn_target_ = key_id;
    cancel_learn_ = true;  // stop a receive that is still waiting for another key
    const int gen = ++learn_gen_;
    // May run inside a key's own click callback: rebuild the grid afterwards.
    lv_async_call([](void* p) { static_cast<Tab5IrRemotePage*>(p)->RefreshKeys(); }, this);
    ir::Device dev;
    ir::Store::GetInstance().GetDevice(device, &dev);
    std::string hint = "正在学习《" + name + "》：请用原装遥控器对准 Tab5 背面，按一下《" + name + "》";
    if (dev.type == "ac")
        hint += "(空调码带完整状态，先在原装遥控器上调好模式和温度)";
    SetStatusLocked(hint);
    Post([this, device, key_id, name, auto_advance, gen] {
        if (gen != learn_gen_.load())
            return;  // superseded by a newer tap
        cancel_learn_ = false;
        const auto cfg = ir::Store::GetInstance().GetConfig();
        std::vector<uint16_t> t;
        std::string error;
        const bool ok = IrService::GetInstance().Learn(cfg.rx_gpio, 20000, &t, &error, &cancel_learn_);
        if (gen != learn_gen_.load())
            return;
        if (!ok) {
            DisplayLockGuard lock(Board::GetInstance().GetDisplay());
            learn_target_ = -1;
            SetStatusLocked("《" + name + "》没学到：" + error + "。点这个键可以重新学习");
            lv_async_call([](void* p) { static_cast<Tab5IrRemotePage*>(p)->RefreshKeys(); }, this);
            return;
        }
        ir::Store::GetInstance().SetTimings(device, key_id, t);
        // Next unlearned key after this one (guided mode).
        ir::Device d;
        ir::Store::GetInstance().GetDevice(device, &d);
        int next_id = -1;
        std::string next_name;
        if (auto_advance) {
            bool after = false;
            for (int pass = 0; pass < 2 && next_id < 0; ++pass)
                for (const auto& k : d.keys) {
                    if (k.id == key_id) {
                        after = true;
                        continue;
                    }
                    if ((after || pass == 1) && !k.learned()) {
                        next_id = k.id;
                        next_name = k.name;
                        break;
                    }
                }
        }
        DisplayLockGuard lock(Board::GetInstance().GetDisplay());
        const std::string done = "已学会《" + name + "》";
        if (next_id > 0) {
            struct Next {
                Tab5IrRemotePage* page;
                int id;
                std::string name;
                std::string done;
            };
            lv_async_call(
                [](void* p) {
                    auto* n = static_cast<Next*>(p);
                    n->page->RefreshDevices();
                    if (n->page->learn_mode_)
                        n->page->Learn(n->id, n->name, true);
                    n->page->SetStatusLocked(n->done + "。下一个：请按原装遥控器的《" + n->name + "》");
                    delete n;
                },
                new Next{this, next_id, next_name, done});
        } else {
            learn_mode_ = false;
            learn_target_ = -1;
            SetStatusLocked(done + (auto_advance ? "。这个设备的键都学完了，点亮的键可直接发送" : "，现在可以点它发送"));
            lv_async_call([](void* p) { static_cast<Tab5IrRemotePage*>(p)->RefreshDevices(); }, this);
        }
    });
}

void Tab5IrRemotePage::Send(int key_id, const std::string& name) {
    const int device = current_device_;
    Post([this, device, key_id, name] {
        const auto cfg = ir::Store::GetInstance().GetConfig();
        std::vector<uint16_t> t;
        uint32_t carrier = 38000;
        int repeat = 1;
        if (!ir::Store::GetInstance().GetTimings(device, key_id, &t, &carrier, &repeat)) {
            SetStatus("《" + name + "》没有键码");
            return;
        }
        const bool ok = IrService::GetInstance().Send(cfg.tx_gpio, t.data(), t.size(), cfg.active_high, carrier, repeat);
        SetStatus(ok ? "已发送《" + name + "》" : "发送失败：红外发射没有响应");
    });
}

void Tab5IrRemotePage::SelfTest(bool interactive) {
    if (interactive)
        SetStatusLocked("自检 1/2：请用任意遥控器对准 Tab5 背面按一下按键…");
    Post([this, interactive] {
        auto cfg = ir::Store::GetInstance().GetConfig();
        std::string rx_result;
        if (interactive) {
            std::vector<uint16_t> t;
            std::string error;
            rx_result = IrService::GetInstance().Learn(cfg.rx_gpio, 8000, &t, &error)
                            ? "接收正常(收到 " + std::to_string(t.size()) + " 段)"
                            : "接收失败：" + error;
            SetStatus("自检 2/2：检测发射…  " + rx_result);
        }
        int heard = -1;
        if (IrService::GetInstance().LoopbackHeard(cfg.tx_gpio, cfg.rx_gpio, true))
            heard = 1;
        else if (IrService::GetInstance().LoopbackHeard(cfg.tx_gpio, cfg.rx_gpio, false))
            heard = 0;
        if (heard >= 0) {
            cfg.active_high = heard == 1;
            cfg.polarity_checked = true;
            ir::Store::GetInstance().SetConfig(cfg);
        }
        ESP_LOGI(TAG, "self test: rx=%s tx=%s", rx_result.c_str(),
                 heard < 0 ? "not heard" : (heard ? "active-high" : "active-low"));
        if (!interactive) {
            if (heard >= 0)
                SetStatus("红外发射自检通过。点亮的按键可直接发送；灰色的键需要先学习");
            return;
        }
        std::string tx_result = heard < 0 ? "发射：本机没听到回波(发射头朝外时正常，可直接对电器试)"
                                          : std::string("发射正常(") + (heard ? "高电平" : "低电平") + "驱动)";
        SetStatus(rx_result + "；" + tx_result);
    });
}
