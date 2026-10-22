// 每日推送 page shell: top bar, the four boards on the left, 今日学习, 今日待办 and 往日回顾.
// 今日运势 lives in tab5_nabo_fortune_view.cc. Views are rebuilt on change; button
// callbacks only record what changed and defer the rebuild (a callback must not delete
// the object that is delivering its own event).
#include "tab5_nabo_push_page.h"

#include <algorithm>
#include <cstdio>

#include "tab5_nabo_ui.h"

LV_FONT_DECLARE(qd_font_cjk_28);
LV_IMAGE_DECLARE(nabo_push_avatar);

using namespace tab5_nabo;
using namespace tab5_nabo_ui;

namespace {

std::string Join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const auto& part : parts)
        out += (out.empty() ? "" : separator) + part;
    return out;
}

struct KindStyle {
    const char* label;
    uint32_t color;
};
KindStyle StyleOf(Kind kind) {
    switch (kind) {
    case Kind::Paper: return {"文献", kPurple};
    case Kind::Word: return {"单词", kBlue};
    case Kind::Point: return {"知识点", kGreen};
    default: return {"消息", kSoft};
    }
}

std::string Clock(const std::string& ts) { return ts.size() >= 16 ? ts.substr(11, 5) : ""; }

}  // namespace

Tab5NaboPushPage::Tab5NaboPushPage(lv_obj_t* parent, Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
    root_ = Panel(parent, 0, 0, 1280, 720, kBg, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    Build();
}

const lv_font_t* Tab5NaboPushPage::Text() const {
    const lv_font_t* font = callbacks_.text_font ? callbacks_.text_font() : nullptr;
    return font ? font : &qd_font_cjk_28;
}

lv_obj_t* Tab5NaboPushPage::Txt(lv_obj_t* parent, const std::string& text, uint32_t color, int x,
                                int y, int w, const lv_font_t* font) {
    return Label(parent, text, font ? font : Text(), color, x, y, w);
}

void Tab5NaboPushPage::Build() {
    Txt(root_, "每日推送", kText, 32, 24, 180);
    date_ = Txt(root_, "", kMuted, 200, 24, 600);
    badge_ = Panel(root_, 916, 20, 48, 48, kOrange, LV_RADIUS_CIRCLE);
    badge_label_ = Label(badge_, "", &lv_font_montserrat_20, kInk, 0, 0, 48);
    lv_obj_set_style_text_align(badge_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(badge_label_);
    auto* read_all = Button(root_, "全部已读", Text(), 980, 20, 160, 48, kBg, kOrange, OnReadAll, this);
    lv_obj_set_style_border_color(read_all, Hex(kOrange), 0);
    lv_obj_set_style_border_width(read_all, 2, 0);
    Button(root_, "返回", Text(), 1152, 20, 96, 48, kChip, kText, OnBack, this);
    rail_ = Panel(root_, 24, 96, 208, 400, kBg, 0);

    // A small NABO under the boards, breathing gently; the caption follows the newest push.
    auto* avatar_card = Panel(root_, 24, 504, 208, 208, kCard, 18);
    lv_obj_add_flag(avatar_card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    auto* avatar = lv_image_create(avatar_card);
    lv_image_set_src(avatar, &nabo_push_avatar);
    lv_obj_set_pos(avatar, 37, -6);
    lv_anim_t breath;
    lv_anim_init(&breath);
    lv_anim_set_var(&breath, avatar);
    lv_anim_set_values(&breath, 0, -6);
    lv_anim_set_duration(&breath, 1800);
    lv_anim_set_reverse_duration(&breath, 1800);
    lv_anim_set_repeat_count(&breath, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&breath, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&breath, [](void* obj, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), value, 0);
    });
    lv_anim_start(&breath);
    avatar_caption_ = Label(avatar_card, "", &qd_font_cjk_28, kSoft, 0, 168, 208);
    lv_obj_set_style_text_align(avatar_caption_, LV_TEXT_ALIGN_CENTER, 0);
    area_ = Panel(root_, 244, 96, 1024, 624, kBg, 0);
}

void Tab5NaboPushPage::Show() {
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    if (callbacks_.refresh)
        callbacks_.refresh();
    rendered_version_ = 0;
    Render();
}

void Tab5NaboPushPage::Hide() { lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }

bool Tab5NaboPushPage::IsVisible() const { return !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN); }

void Tab5NaboPushPage::ShowBoard(Board board) {
    board_ = board;
    Show();
}

void Tab5NaboPushPage::SetData(SharedData data) {
    data_ = std::move(data);
    if (IsVisible() && data_ && data_->version != rendered_version_)
        Defer(this, [](void* self) { static_cast<Tab5NaboPushPage*>(self)->Render(); });
}

// What the user is looking at counts as read: today's fortune, the open study card and
// every to-do on the list.
void Tab5NaboPushPage::MarkVisibleRead() {
    if (!data_)
        return;
    auto& store = Store::GetInstance();
    bool marked = false;
    const auto mark = [&](const Item* item) {
        if (item && !data_->marks.read.count(item->id)) {
            store.SetMark(Mark::Read, item->id, true);
            marked = true;
        }
    };
    if (board_ == kFortune)
        mark(data_->TodayFortune(today_));
    if (board_ == kStudy)
        for (const auto* item : data_->TodayStudy(today_))
            if (item->id == study_id_)
                mark(item);
    if (board_ == kTodo)
        for (const auto* item : data_->TodayTodos(today_))
            mark(item);
    if (marked)
        data_ = store.Get();
}

void Tab5NaboPushPage::Render() {
    if (!data_)
        data_ = Store::GetInstance().Get();
    today_ = Today();
    if (today_.empty()) {
        // Before SNTP sets the clock, treat the newest pushed day as today.
        for (const auto& item : data_->items)
            today_ = std::max(today_, item.Day());
    }
    if (board_ == kStudy && study_id_.empty()) {
        const auto study = data_->TodayStudy(today_);
        if (!study.empty())
            study_id_ = study.front()->id;
    }
    MarkVisibleRead();
    rendered_version_ = data_->version;
    lv_label_set_text(date_, (DayTitle(today_) + " · Nabo").c_str());
    const int unread = data_->Unread(today_);
    lv_label_set_text(badge_label_, std::to_string(unread).c_str());
    SetHidden(badge_, unread == 0);

    RenderRail();
    UpdateAvatar();
    lv_obj_clean(area_);
    keys_.clear();
    switch (board_) {
    case kFortune: RenderFortune(area_); break;
    case kStudy: RenderStudy(area_); break;
    case kTodo: RenderTodo(area_); break;
    case kReview: RenderReview(area_); break;
    }
    if (!data_->online && data_->ever_synced) {
        auto* note = Panel(area_, 12, 588, 1000, 36, kNote, 18);
        Txt(note, "中转站暂时连不上，显示的是上次同步的内容", kNoteText, 18, 2, 960);
    }
}

void Tab5NaboPushPage::RenderRail() {
    lv_obj_clean(rail_);
    const Item* fortune = data_->TodayFortune(today_);
    std::string fortune_sub = "暂无";
    if (fortune) {
        const Fortune f = ParseFortune(fortune->body);
        fortune_sub = (f.score ? Score(f.score) + " " : std::string()) + f.summary;
    }
    const auto study = data_->TodayStudy(today_);
    const auto todos = data_->TodayTodos(today_);
    int done = 0;
    bool study_new = false, todo_new = false;
    for (const auto* item : study)
        study_new |= !data_->marks.read.count(item->id);
    for (const auto* item : todos) {
        done += data_->marks.done.count(item->id) ? 1 : 0;
        todo_new |= !data_->marks.read.count(item->id);
    }
    const struct {
        const char* name;
        std::string sub;
        bool dot;
    } rows[] = {
        {"今日运势", fortune_sub, fortune && !data_->marks.read.count(fortune->id)},
        {"今日学习", study.empty() ? "暂无" : std::to_string(study.size()) + " 条", study_new},
        {"今日待办", std::to_string(done) + " / " + std::to_string(todos.size()) + " 已完成", todo_new},
        {"往日回顾", "本地按天生成", false},
    };
    for (int i = 0; i < 4; ++i) {
        const bool active = board_ == i;
        auto* button = Panel(rail_, 0, i * 102, 208, 92, active ? kOrange : kCard, 18);
        Clickable(button, OnBoard, this, i);
        Txt(button, rows[i].name, active ? kInk : kText, 18, 10, 172);
        auto* sub = Txt(button, rows[i].sub, active ? kInk : kMuted, 18, 50, 172);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_height(sub, 36);
        if (rows[i].dot)
            Panel(button, 186, 12, 10, 10, kOrange, LV_RADIUS_CIRCLE);
    }
}

void Tab5NaboPushPage::RenderStudy(lv_obj_t* area) {
    const auto study = data_->TodayStudy(today_);
    if (study.empty()) {
        Txt(area, "今天还没有学习推送", kMuted, 12, 220, 1000);
        return;
    }
    auto* list = Scroller(area, 0, 0, 412, 624);
    lv_obj_set_style_pad_row(list, 8, 0);
    const Item* current = study.front();
    for (const auto* item : study) {
        if (item->id == study_id_)
            current = item;
        const bool selected = item->id == study_id_;
        const bool read = data_->marks.read.count(item->id) > 0;
        const KindStyle style = StyleOf(item->kind());
        keys_.push_back(item->id);
        auto* row = Panel(list, 0, 0, 388, LV_SIZE_CONTENT, selected ? kSelected : kRow, 18);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(row, 16, 0);
        lv_obj_set_style_pad_row(row, 8, 0);
        Clickable(row, OnStudy, this, int(keys_.size() - 1));
        auto* head = Row(row, 356);
        Tag(head, style.label, style.color, 0, 0, Text());
        std::string meta = Clock(item->ts);
        if (data_->marks.learned.count(item->id))
            meta += " · 已学会";
        Txt(head, meta, kMuted, 0, 0, LV_SIZE_CONTENT);
        auto* title = Txt(row, item->title, selected ? kOrange : (read ? kSoft : kText), 0, 0, 356);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_height(title, 72);
    }
    study_id_ = current->id;
    const KindStyle style = StyleOf(current->kind());
    auto* card = Panel(area, 424, 0, 588, 600, kCard, 22);
    Tag(card, style.label, style.color, 30, 26);
    Txt(card, "来自 Nabo · 今天 " + Clock(current->ts), kMuted, 150, 26, 400);
    Txt(card, current->title, kText, 30, 76, 528);
    auto* body = Scroller(card, 30, 160, 528, 330);
    Txt(body, current->body.empty() ? "暂无正文" : current->body, kBody, 0, 0, 512);
    keys_.push_back(current->id);
    const int tag = int(keys_.size() - 1);
    const bool learned = data_->marks.learned.count(current->id) > 0;
    const bool fav = data_->marks.fav.count(current->id) > 0;
    Button(card, learned ? "已学会" : "学会了", Text(), 30, 512, 160, 56,
           learned ? kChip : kOrange, learned ? kGreen : kInk, OnLearned, this, tag);
    Button(card, fav ? "已收藏" : "收藏", Text(), 206, 512, 150, 56, kChip,
           fav ? kPink : kText, OnFav, this, tag);
}

void Tab5NaboPushPage::RenderTodo(lv_obj_t* area) {
    // TodayTodos is in time order; open items first, finished ones sink to the bottom.
    std::vector<const Item*> todos, finished_items;
    for (const auto* item : data_->TodayTodos(today_))
        (data_->marks.done.count(item->id) ? finished_items : todos).push_back(item);
    const int done = int(finished_items.size());
    todos.insert(todos.end(), finished_items.begin(), finished_items.end());
    Txt(area, "已完成 " + std::to_string(done) + " / " + std::to_string(todos.size()), kText, 12, 4, 260);
    auto* track = Panel(area, 280, 18, 732, 10, kChip, 5);
    if (!todos.empty() && done)
        Panel(track, 0, 0, 732 * done / int(todos.size()), 10, kOrange, 5);
    if (todos.empty()) {
        Txt(area, "今天还没有待办推送", kMuted, 12, 220, 1000);
        return;
    }
    auto* list = Scroller(area, 12, 54, 1000, 570);
    lv_obj_set_style_pad_row(list, 8, 0);
    for (const auto* item : todos) {
        const bool finished = data_->marks.done.count(item->id) > 0;
        keys_.push_back(item->id);
        auto* row = Panel(list, 0, 0, 1000, 80, kCard, 16);
        if (item->high() && !finished)
            Panel(row, 0, 14, 5, 52, kOrange, 3);
        auto* box = Panel(row, 18, 16, 48, 48, finished ? kOrange : kCard, 14);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, Hex(finished ? kOrange : 0x4A5363), 0);
        Clickable(box, OnTodo, this, int(keys_.size() - 1));
        lv_obj_set_ext_click_area(box, 12);
        if (finished) {
            auto* tick = Label(box, LV_SYMBOL_OK, &lv_font_montserrat_20, kInk, 0, 0, 24);
            lv_obj_center(tick);
        }
        const int minutes = TodoMinutes(*item);
        char time[24] = "—";
        if (minutes < 24 * 60)
            std::snprintf(time, sizeof(time), "%02d:%02d", minutes / 60, minutes % 60);
        Txt(row, time, finished ? kFaint : (minutes < 24 * 60 ? kOrange : kMuted), 86, 22, 90);
        auto* label = Txt(row, TodoLabel(*item), finished ? kFaint : kText, 186, 22, 640);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, 40);
        if (finished)
            lv_obj_set_style_text_decor(label, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        Txt(row, "来自 Nabo", kMuted, 850, 22, 140);
    }
}

void Tab5NaboPushPage::RenderReview(lv_obj_t* area) {
    auto days = data_->Review(today_);
    if (fav_only_)
        days.erase(std::remove_if(days.begin(), days.end(), [](const DaySummary& d) { return !d.fav; }),
                   days.end());
    Pill(area, "按天", !fav_only_, 110, 48, OnReviewTab, 0);
    lv_obj_set_pos(lv_obj_get_child(area, -1), 12, 0);
    Pill(area, "只看收藏", fav_only_, 150, 48, OnReviewTab, 1);
    lv_obj_set_pos(lv_obj_get_child(area, -1), 132, 0);
    if (days.empty()) {
        Txt(area, fav_only_ ? "还没有收藏的日子" : "还没有往日记录，过了今天就会出现在这里", kMuted, 12, 220, 1000);
        return;
    }
    const DaySummary* current = &days.front();
    for (const auto& day : days)
        if (day.day == day_)
            current = &day;
    day_ = current->day;
    auto* list = Scroller(area, 0, 62, 412, 562);
    lv_obj_set_style_pad_row(list, 8, 0);
    const std::string week_ago = ShiftDay(today_, -7);
    for (const auto& day : days) {
        const bool selected = day.day == day_;
        keys_.push_back(day.day);
        auto* row = Panel(list, 0, 0, 388, 104, selected ? kSelected : kRow, 18);
        Clickable(row, OnDay, this, int(keys_.size() - 1));
        Label(row, day.day.substr(8, 2), &lv_font_montserrat_48, selected ? kOrange : kText, 14, 8, 84);
        Txt(row, day.day == week_ago ? "一周前" : Weekday(day.day), kMuted, 14, 62, 90);
        Txt(row, "运势 " + (day.score ? Score(day.score) : std::string("—")), kText, 112, 14, 160);
        if (day.fav)
            Txt(row, "已收藏", kPink, 280, 14, 100);
        Txt(row, "学 " + std::to_string(day.learned) + "/" + std::to_string(day.study_total) + " · 办 " +
                     std::to_string(day.done) + "/" + std::to_string(day.todo_total),
            kMuted, 112, 56, 264);
    }
    auto* card = Panel(area, 424, 0, 588, 600, kCard, 22);
    Txt(card, DayTitle(current->day), kText, 30, 24, 380);
    keys_.push_back(current->day);
    Button(card, current->fav ? "已收藏" : "收藏这天", Text(), 390, 18, 170, 52, kChip,
           current->fav ? kPink : kText, OnDayFav, this, int(keys_.size() - 1));
    auto* body = Scroller(card, 30, 84, 528, 496);
    lv_obj_set_style_pad_row(body, 6, 0);
    Txt(body, "Tab5 按本地记录自动生成", kMuted, 0, 0, 512);
    Txt(body, "运势", kOrange, 0, 0, 512);
    Txt(body, current->score ? Score(current->score) + "  " + current->fortune : "暂无", kText, 0, 0, 512);
    Txt(body, "学习 " + std::to_string(current->learned) + " / " + std::to_string(current->study_total) + " 学会",
        kPurple, 0, 0, 512);
    Txt(body, current->study.empty() ? "暂无" : "· " + Join(current->study, "\n· "), kText, 0, 0, 512);
    Txt(body, "待办 " + std::to_string(current->done) + " / " + std::to_string(current->todo_total) + " 完成",
        kGreen, 0, 0, 512);
    Txt(body, current->todo_total == 0 ? "暂无"
              : current->undone.empty() ? "全部完成"
                                        : "没完成：\n· " + Join(current->undone, "\n· "),
        kSoft, 0, 0, 512);
}

void Tab5NaboPushPage::UpdateAvatar() {
    const Item* newest = nullptr;
    bool any = false;
    for (const auto& item : data_->items) {
        if (item.Day() != today_ || item.kind() == Kind::Other)
            continue;
        any = true;
        if (!data_->marks.read.count(item.id) && (!newest || item.rev > newest->rev))
            newest = &item;
    }
    const std::string mood = newest ? newest->mood : "";
    const char* text = !any                 ? "等 Nabo 推送"
                       : !newest            ? "今天都看完了"
                       : mood == "alert"    ? "有一条要紧的"
                       : mood == "rain"     ? "今天有雨"
                       : mood == "happy"    ? "有个好消息"
                       : mood == "thinking" ? "一起想一想"
                                            : "有新的推送";
    lv_label_set_text(avatar_caption_, text);
}

// ---- events -------------------------------------------------------------

namespace {
Tab5NaboPushPage* Self(lv_event_t* e) { return static_cast<Tab5NaboPushPage*>(lv_event_get_user_data(e)); }
}  // namespace

void Tab5NaboPushPage::OnBack(lv_event_t* e) {
    if (auto* self = Self(e); self->callbacks_.back)
        self->callbacks_.back();
}

void Tab5NaboPushPage::OnReadAll(lv_event_t* e) {
    auto* self = Self(e);
    Store::GetInstance().MarkAllRead(self->today_);
    self->SetData(Store::GetInstance().Get());
}

void Tab5NaboPushPage::OnBoard(lv_event_t* e) {
    auto* self = Self(e);
    self->board_ = Board(TagOf(e));
    self->rendered_version_ = 0;
    Defer(self, [](void* p) { static_cast<Tab5NaboPushPage*>(p)->Render(); });
}

void Tab5NaboPushPage::OnModule(lv_event_t* e) {
    auto* self = Self(e);
    self->module_ = TagOf(e);
    Defer(self, [](void* p) { static_cast<Tab5NaboPushPage*>(p)->Render(); });
}

void Tab5NaboPushPage::OnStudy(lv_event_t* e) {
    auto* self = Self(e);
    self->study_id_ = self->keys_.at(TagOf(e));
    Defer(self, [](void* p) { static_cast<Tab5NaboPushPage*>(p)->Render(); });
}

namespace {
void Toggle(Tab5NaboPushPage* self, Mark mark, const KeySet& set, const std::string& key) {
    Store::GetInstance().SetMark(mark, key, !set.count(key));
    self->SetData(Store::GetInstance().Get());
}
}  // namespace

void Tab5NaboPushPage::OnLearned(lv_event_t* e) {
    auto* self = Self(e);
    Toggle(self, Mark::Learned, self->data_->marks.learned, self->keys_.at(TagOf(e)));
}

void Tab5NaboPushPage::OnFav(lv_event_t* e) {
    auto* self = Self(e);
    Toggle(self, Mark::Fav, self->data_->marks.fav, self->keys_.at(TagOf(e)));
}

void Tab5NaboPushPage::OnTodo(lv_event_t* e) {
    auto* self = Self(e);
    Toggle(self, Mark::Done, self->data_->marks.done, self->keys_.at(TagOf(e)));
}

void Tab5NaboPushPage::OnDay(lv_event_t* e) {
    auto* self = Self(e);
    self->day_ = self->keys_.at(TagOf(e));
    Defer(self, [](void* p) { static_cast<Tab5NaboPushPage*>(p)->Render(); });
}

void Tab5NaboPushPage::OnDayFav(lv_event_t* e) {
    auto* self = Self(e);
    Toggle(self, Mark::FavDay, self->data_->marks.fav_days, self->keys_.at(TagOf(e)));
}

void Tab5NaboPushPage::OnReviewTab(lv_event_t* e) {
    auto* self = Self(e);
    self->fav_only_ = TagOf(e) == 1;
    Defer(self, [](void* p) { static_cast<Tab5NaboPushPage*>(p)->Render(); });
}

// ---- builders -----------------------------------------------------------

lv_obj_t* Tab5NaboPushPage::Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
    return Panel(parent, x, y, w, h, color, radius);
}

lv_obj_t* Tab5NaboPushPage::Pill(lv_obj_t* parent, const std::string& text, bool active, int w, int h,
                                 lv_event_cb_t cb, intptr_t tag) {
    return Button(parent, text, Text(), 0, 0, w, h, active ? kOrange : kChip, active ? kInk : kBody, cb,
                  this, int(tag));
}

lv_obj_t* Tab5NaboPushPage::Scroller(lv_obj_t* parent, int x, int y, int w, int h) {
    return Column(parent, x, y, w, h);
}

lv_obj_t* Tab5NaboPushPage::Chips(lv_obj_t* parent, const char* label, uint32_t color,
                                  const std::vector<std::string>& words, int w) {
    auto* row = Row(parent, w);
    auto* badge = Panel(row, 0, 0, 48, 48, kCard, 14);
    lv_obj_set_style_bg_color(badge, Hex(color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    auto* text = Txt(badge, label, color, 0, 0, 48);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(text);
    if (words.empty())
        Txt(row, "暂无", kMuted, 0, 0, w - 70);
    for (const auto& word : words) {
        auto* chip = Panel(row, 0, 0, LV_SIZE_CONTENT, 48, kChip, 24);
        lv_obj_set_style_pad_hor(chip, 18, 0);
        auto* label_obj = Txt(chip, word, kText, 0, 0, LV_SIZE_CONTENT);
        lv_obj_center(label_obj);
    }
    return row;
}
