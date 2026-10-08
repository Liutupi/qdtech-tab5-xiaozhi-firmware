#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "tab5_home_hub.h"

// 米家中控 page: combined scenes (卧室电脑, 家庭影院, ...) on top, then every Home Assistant
// device grouped by room. The learning IR remote stays one tap away. All LVGL calls happen
// with the display lock held.
class Tab5HomePage {
public:
    Tab5HomePage(lv_obj_t* parent, std::function<void()> back, std::function<void()> open_ir);
    ~Tab5HomePage();
    void Show();
    void Hide();
    bool IsVisible() const;
    // Display lock held. Rebuilds the grid only when the catalog changed.
    void SetStatus(const tab5_home::Status& status);

private:
    enum Kind { kBack, kIr, kRefresh, kSceneOn, kSceneOff, kToggle, kTempDown, kTempUp };
    struct Tag {
        Tab5HomePage* page;
        Kind kind;
        std::string id;     // entity or scene id
        std::string label;  // spoken/status name
        float value = 0;    // current target temperature for climate
        bool on = false;
    };

    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* scenes_ = nullptr;
    lv_obj_t* devices_ = nullptr;
    std::function<void()> back_;
    std::function<void()> open_ir_;
    std::vector<std::unique_ptr<Tag>> tags_;   // grid buttons, rebuilt with the grid
    std::vector<std::unique_ptr<Tag>> fixed_;  // header buttons
    std::shared_ptr<const tab5_home::Catalog> rendered_;
    std::string status_text_;

    void Build();
    void Render(const tab5_home::Catalog& catalog);
    void RenderScene(const tab5_home::Scene& scene);
    void RenderDevice(lv_obj_t* parent, const tab5_home::Device& device);
    void SetStatusText(const std::string& text);
    lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, int w, int h, uint32_t bg, Tag* tag);
    Tag* NewTag(Kind kind, std::string id = {}, std::string label = {}, bool fixed = false);
    void OnTag(const Tag& tag);
    static void OnEvent(lv_event_t* e);
};
