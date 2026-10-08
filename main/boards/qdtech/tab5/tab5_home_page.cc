#include "tab5_home_page.h"

#include <cmath>
#include <cstdio>

#include "application.h"
#include "board.h"
#include "display.h"

LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_cjk_28);

namespace {

constexpr uint32_t kBg = 0x0d1b2b;
constexpr uint32_t kPanel = 0x122b43;
constexpr uint32_t kTile = 0x173650;
constexpr uint32_t kTileOn = 0x1d6f8a;
constexpr uint32_t kOffline = 0x1a2633;
constexpr uint32_t kText = 0xf5f9fd;
constexpr uint32_t kMuted = 0x9bb7ca;
constexpr uint32_t kAccent = 0x86a8e8;
constexpr int kTileW = 276, kTileH = 128;

lv_obj_t* Box(lv_obj_t* parent, int w, int h, uint32_t color, int radius) {
    auto* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* Text(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int x, int y,
               int w) {
    auto* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

const char* ModeName(const std::string& state) {
    if (state == "cool")
        return "制冷";
    if (state == "heat")
        return "制热";
    if (state == "dry")
        return "除湿";
    if (state == "fan_only")
        return "送风";
    if (state == "auto" || state == "heat_cool")
        return "自动";
    return "已开";
}

// Status line for a tile: 已开 / 已关 / 离线 / 制冷 26度 · 室温 27.5度 / 亮度 78%.
std::string Describe(const tab5_home::Device& d) {
    using tab5_home::Kind;
    if (!d.available())
        return "离线";
    char buffer[96];
    if (d.kind() == Kind::kClimate) {
        const std::string mode = d.on() ? ModeName(std::string(d.state.data(), d.state.size())) : "已关";
        int n = std::snprintf(buffer, sizeof(buffer), "%s", mode.c_str());
        if (d.on() && d.target > -999)
            n += std::snprintf(buffer + n, sizeof(buffer) - n, " %.0f度", d.target);
        if (d.current > -999)
            std::snprintf(buffer + n, sizeof(buffer) - n, " · 室温 %.1f度", d.current);
        return buffer;
    }
    if (d.kind() == Kind::kLight && d.on() && d.brightness >= 0) {
        std::snprintf(buffer, sizeof(buffer), "已开 · 亮度 %d%%", int(std::lround(d.brightness * 100 / 255.0)));
        return buffer;
    }
    if (d.kind() == Kind::kMedia && d.state == "playing")
        return "播放中";
    return d.on() ? "已开" : "已关";
}

}  // namespace

Tab5HomePage::Tab5HomePage(lv_obj_t* parent, std::function<void()> back, std::function<void()> open_ir)
    : back_(std::move(back)), open_ir_(std::move(open_ir)) {
    root_ = Box(parent, 1280, 720, kBg, 0);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    Build();
}

Tab5HomePage::~Tab5HomePage() {
    if (root_)
        lv_obj_delete(root_);
}

void Tab5HomePage::Show() {
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    tab5_home::Hub::GetInstance().SetPageVisible(true);
    SetStatus(tab5_home::Hub::GetInstance().Current());
}

void Tab5HomePage::Hide() {
    if (lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN))
        return;
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    tab5_home::Hub::GetInstance().SetPageVisible(false);
}

bool Tab5HomePage::IsVisible() const { return !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN); }

void Tab5HomePage::Build() {
    Text(root_, "米家中控", &qd_font_lxgw_36, kText, 52, 24, 400);
    status_ = Text(root_, "正在连接 NAS…", &qd_font_cjk_28, kMuted, 54, 78, 700);
    auto* refresh = MakeButton(root_, "刷新", 130, 60, 0x254c66, NewTag(kRefresh, {}, {}, true));
    lv_obj_set_pos(refresh, 650, 20);
    auto* ir = MakeButton(root_, "红外遥控", 200, 60, 0x254c66, NewTag(kIr, {}, {}, true));
    lv_obj_set_pos(ir, 796, 20);
    auto* back = MakeButton(root_, "返回", 200, 60, 0x254c66, NewTag(kBack, {}, {}, true));
    lv_obj_set_pos(back, 1028, 20);

    // One row of combined scenes; scrolls sideways when there are many.
    scenes_ = Box(root_, 1176, 136, kBg, 0);
    lv_obj_set_pos(scenes_, 52, 124);
    lv_obj_add_flag(scenes_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scenes_, LV_DIR_HOR);
    lv_obj_set_flex_flow(scenes_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(scenes_, 16, 0);

    devices_ = Box(root_, 1176, 440, kPanel, 22);
    lv_obj_set_pos(devices_, 52, 270);
    lv_obj_add_flag(devices_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(devices_, LV_DIR_VER);
    lv_obj_set_flex_flow(devices_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(devices_, 16, 0);
    lv_obj_set_style_pad_row(devices_, 14, 0);
    lv_obj_set_style_pad_column(devices_, 14, 0);
}

void Tab5HomePage::SetStatusText(const std::string& text) {
    if (text == status_text_)
        return;
    status_text_ = text;
    lv_label_set_text(status_, text.c_str());
}

void Tab5HomePage::SetStatus(const tab5_home::Status& status) {
    if (status.catalog && status.catalog != rendered_) {
        rendered_ = status.catalog;
        Render(*rendered_);
    }
    if (!status.message.empty())
        SetStatusText(status.message);
    else if (rendered_)
        SetStatusText("Home Assistant 已连接 · " + std::to_string(rendered_->devices.size()) + " 个设备");
}

void Tab5HomePage::Render(const tab5_home::Catalog& catalog) {
    lv_obj_clean(scenes_);
    lv_obj_clean(devices_);
    tags_.clear();
    for (const auto& scene : catalog.scenes)
        RenderScene(scene);
    if (catalog.scenes.empty())
        Text(scenes_, "还没有组合场景", &qd_font_cjk_28, kMuted, 0, 40, 600);

    // Devices arrive sorted by room; start a full-width room heading at each change.
    std::string room = "\x01";
    for (const auto& d : catalog.devices) {
        if (d.kind() == tab5_home::Kind::kScene || d.kind() == tab5_home::Kind::kOther)
            continue;
        const std::string area(d.area.data(), d.area.size());
        if (area != room) {
            room = area;
            auto* heading = Text(devices_, area.empty() ? "其他" : area.c_str(), &qd_font_cjk_28, kAccent, 0, 0, 1100);
            lv_obj_set_height(heading, 38);
            lv_obj_add_flag(heading, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        }
        RenderDevice(devices_, d);
    }
}

void Tab5HomePage::RenderScene(const tab5_home::Scene& scene) {
    const std::string id(scene.id.data(), scene.id.size()), name(scene.name.data(), scene.name.size());
    auto* card = Box(scenes_, 340, 128, kTile, 22);
    Text(card, name.c_str(), &qd_font_cjk_28, kText, 22, 14, 296);
    Text(card, scene.room.empty() ? "组合场景" : std::string(scene.room.data(), scene.room.size()).c_str(),
         &qd_font_cjk_28, kMuted, 22, 50, 140);
    if (scene.has_on) {
        auto* on = MakeButton(card, "开", 72, 56, 0x2f7a52, NewTag(kSceneOn, id, name));
        lv_obj_set_pos(on, 168, 58);
    }
    if (scene.has_off) {
        auto* off = MakeButton(card, "关", 72, 56, 0x4a2630, NewTag(kSceneOff, id, name));
        lv_obj_set_pos(off, 252, 58);
    }
}

void Tab5HomePage::RenderDevice(lv_obj_t* parent, const tab5_home::Device& d) {
    const bool online = d.available(), on = d.on();
    const std::string id(d.id.data(), d.id.size()), label(d.label.data(), d.label.size());
    auto* tile = Box(parent, kTileW, kTileH, online ? (on ? kTileOn : kTile) : kOffline, 20);
    Text(tile, label.c_str(), &qd_font_cjk_28, online ? kText : 0x6f8496, 18, 12, kTileW - 36);
    Text(tile, Describe(d).c_str(), &qd_font_cjk_28, online ? 0xbfe4f2 : 0x5d7183, 18, 48, kTileW - 36);
    if (!online)
        return;
    if (d.kind() == tab5_home::Kind::kClimate && on && d.target > -999) {
        auto* down = NewTag(kTempDown, id, label);
        down->value = d.target;
        auto* up = NewTag(kTempUp, id, label);
        up->value = d.target;
        lv_obj_set_pos(MakeButton(tile, "-", 56, 44, 0x254c66, down), 18, 80);
        lv_obj_set_pos(MakeButton(tile, "+", 56, 44, 0x254c66, up), 82, 80);
    }
    auto* toggle = NewTag(kToggle, id, label);
    toggle->on = on;
    lv_obj_set_pos(MakeButton(tile, on ? "关闭" : "打开", 104, 44, on ? 0x4a2630 : 0x2f7a52, toggle), kTileW - 122, 80);
}

lv_obj_t* Tab5HomePage::MakeButton(lv_obj_t* parent, const char* text, int w, int h, uint32_t bg, Tag* tag) {
    auto* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    auto* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &qd_font_cjk_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kText), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, OnEvent, LV_EVENT_SHORT_CLICKED, tag);
    return b;
}

Tab5HomePage::Tag* Tab5HomePage::NewTag(Kind kind, std::string id, std::string label, bool fixed) {
    auto tag = std::make_unique<Tag>(Tag{this, kind, std::move(id), std::move(label)});
    Tag* raw = tag.get();
    (fixed ? fixed_ : tags_).push_back(std::move(tag));
    return raw;
}

void Tab5HomePage::OnEvent(lv_event_t* e) {
    const auto* tag = static_cast<const Tag*>(lv_event_get_user_data(e));
    if (tag)
        tag->page->OnTag(*tag);
}

void Tab5HomePage::OnTag(const Tag& tag) {
    auto& hub = tab5_home::Hub::GetInstance();
    // Results arrive on the hub task; show them on the page under the display lock.
    auto report = [this, label = tag.label](bool ok, const std::string& message) {
        const std::string text = ok ? "已执行：" + label : label + "：" + message;
        Application::GetInstance().Schedule([this, text] {
            DisplayLockGuard lock(Board::GetInstance().GetDisplay());
            if (lock.locked())
                SetStatusText(text);
        });
    };
    bool queued = true;
    switch (tag.kind) {
        case kBack:
            if (back_)
                back_();
            return;
        case kIr:
            if (open_ir_)
                open_ir_();
            return;
        case kRefresh:
            hub.RequestRefresh();
            SetStatusText("正在刷新…");
            return;
        case kSceneOn:
        case kSceneOff:
            queued = hub.RunScene(tag.id, tag.kind == kSceneOn, report);
            SetStatusText((tag.kind == kSceneOn ? "正在打开：" : "正在关闭：") + tag.label);
            break;
        case kToggle:
            queued = hub.Control(tag.id, tag.on ? "off" : "on", "", report);
            SetStatusText((tag.on ? "正在关闭：" : "正在打开：") + tag.label);
            break;
        case kTempDown:
        case kTempUp: {
            char value[8];
            std::snprintf(value, sizeof(value), "%.0f", tag.value + (tag.kind == kTempUp ? 1 : -1));
            queued = hub.Control(tag.id, "temperature", value, report);
            SetStatusText(tag.label + " 调到 " + value + " 度");
            break;
        }
    }
    if (!queued)
        SetStatusText("操作太频繁，请稍等");
}
