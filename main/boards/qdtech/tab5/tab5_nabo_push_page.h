#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "tab5_nabo_push.h"

// 每日推送 page: 今日运势 (nine module tabs), 今日学习, 今日待办 and 往日回顾 for the
// one-way NABO pushes kept by tab5_nabo::Store. Every view is rebuilt from the current
// immutable snapshot; marks go back through Store::SetMark. LVGL task only.
class Tab5NaboPushPage {
public:
    struct Callbacks {
        std::function<void()> back;
        std::function<void()> refresh;
        std::function<const lv_font_t*()> text_font;  // full CJK font for pushed text
    };

    Tab5NaboPushPage(lv_obj_t* parent, Callbacks callbacks);
    lv_obj_t* object() const { return root_; }
    void Show();
    void Hide();
    bool IsVisible() const;
    void SetData(tab5_nabo::SharedData data);

    enum Board { kFortune, kStudy, kTodo, kReview };
    // Opens straight on one board (home reminder card taps land here).
    void ShowBoard(Board board);
    // Chooses the board the next Show() opens on.
    void SelectBoard(Board board) { board_ = board; }

private:
    void Build();
    void Render();
    void RenderRail();
    void RenderFortune(lv_obj_t* area);
    void RenderFortuneModule(lv_obj_t* card, const tab5_nabo::Fortune& f);
    void RenderStudy(lv_obj_t* area);
    void RenderTodo(lv_obj_t* area);
    void RenderReview(lv_obj_t* area);
    void MarkVisibleRead();
    void UpdateAvatar();
    const lv_font_t* Text() const;

    // Small builders shared by the views.
    lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius);
    lv_obj_t* Txt(lv_obj_t* parent, const std::string& text, uint32_t color, int x, int y, int w,
                  const lv_font_t* font = nullptr);
    lv_obj_t* Pill(lv_obj_t* parent, const std::string& text, bool active, int w, int h,
                   lv_event_cb_t cb, intptr_t tag);
    lv_obj_t* Scroller(lv_obj_t* parent, int x, int y, int w, int h);
    lv_obj_t* Chips(lv_obj_t* parent, const char* label, uint32_t color,
                    const std::vector<std::string>& words, int w);

    static void OnBack(lv_event_t* e);
    static void OnReadAll(lv_event_t* e);
    static void OnBoard(lv_event_t* e);
    static void OnModule(lv_event_t* e);
    static void OnStudy(lv_event_t* e);
    static void OnLearned(lv_event_t* e);
    static void OnFav(lv_event_t* e);
    static void OnTodo(lv_event_t* e);
    static void OnDay(lv_event_t* e);
    static void OnDayFav(lv_event_t* e);
    static void OnReviewTab(lv_event_t* e);

    Callbacks callbacks_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* date_ = nullptr;
    lv_obj_t* badge_ = nullptr;
    lv_obj_t* badge_label_ = nullptr;
    lv_obj_t* rail_ = nullptr;
    lv_obj_t* avatar_caption_ = nullptr;  // under the small NABO below the boards
    lv_obj_t* area_ = nullptr;
    tab5_nabo::SharedData data_;
    uint32_t rendered_version_ = 0;
    std::string today_;

    Board board_ = kFortune;
    int module_ = 0;
    std::string study_id_;
    std::string day_;
    bool fav_only_ = false;
    std::vector<std::string> keys_;  // ids behind the current view's tagged buttons
};
