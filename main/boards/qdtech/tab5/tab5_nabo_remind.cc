#include "tab5_nabo_remind.h"

#include <cstdio>

namespace tab5_nabo {
namespace {

constexpr int kFortune = 0, kStudy = 1, kTodo = 2, kReview = 3;

int BoardOf(Kind kind) {
    switch (kind) {
    case Kind::Fortune: return kFortune;
    case Kind::Todo: return kTodo;
    default: return kStudy;
    }
}

std::string Clock(int minutes) {
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60 % 24, minutes % 60);
    return text;
}

}  // namespace

const char* MoodAction(const std::string& mood) {
    if (mood == "alert") return "A07";     // 指向右侧
    if (mood == "rain") return "E10";      // 摇头轻叹
    if (mood == "happy") return "E06";     // 闭眼满足笑
    if (mood == "thinking") return "A05";  // 托腮思考
    return "P02";                          // 翻开小书
}

Reminder PickReminder(const Data& data, const LocalClock& clock, const KeySet& dismissed) {
    Reminder r;
    if (clock.today.empty())
        return r;
    const auto offer = [&](const std::string& key) { return !dismissed.count(key); };

    // 1. A to-do about to start, or started within the last hour and still open.
    for (const auto* item : data.TodayTodos(clock.today)) {
        const int at = TodoMinutes(*item);
        if (at >= 24 * 60 || data.marks.done.count(item->id))
            continue;
        const int delta = at - clock.minutes;
        const std::string key = "todo:" + item->id;
        if (delta > 15 || delta < -60 || !offer(key))
            continue;
        r.active = true;
        r.board = kTodo;
        r.title = delta > 0   ? "待办 · " + std::to_string(delta) + " 分钟后开始"
                  : delta == 0 ? std::string("待办 · 现在开始")
                               : "待办 · 已过 " + std::to_string(-delta) + " 分钟";
        r.body = Clock(at) + " " + TodoLabel(*item);
        r.key = key;
        r.action = "A07";
        return r;
    }

    // 2. The newest unread high-priority push of today.
    const Item* high = nullptr;
    for (const auto& item : data.items)
        if (item.high() && item.Day() == clock.today && !data.marks.read.count(item.id) &&
            item.kind() != Kind::Other && offer("high:" + item.id) && (!high || item.rev > high->rev))
            high = &item;
    if (high) {
        r.active = true;
        r.board = BoardOf(high->kind());
        r.title = "重要推送";
        r.body = high->kind() == Kind::Todo ? TodoLabel(*high) : high->title;
        r.key = "high:" + high->id;
        r.action = MoodAction(high->mood.empty() ? "alert" : high->mood);
        return r;
    }

    // 3. Morning: today's fortune until it is read or noon.
    const Item* fortune = data.TodayFortune(clock.today);
    if (fortune && clock.minutes >= 6 * 60 && clock.minutes < 12 * 60 &&
        !data.marks.read.count(fortune->id) && offer("fortune:" + clock.today)) {
        const Fortune f = ParseFortune(fortune->body);
        r.active = true;
        r.board = kFortune;
        r.title = f.score ? "今日运势 · " + std::to_string(f.score) + "/5" : std::string("今日运势");
        r.body = f.summary.empty() ? fortune->title : f.summary;
        if (!f.yi.empty())
            r.body += " · 宜" + f.yi.front();
        r.key = "fortune:" + clock.today;
        r.action = "E06";
        return r;
    }

    // 4. Evening: study cards not marked 学会了 yet.
    if (clock.minutes >= 20 * 60 && clock.minutes < 23 * 60 && offer("study:" + clock.today)) {
        int left = 0;
        const Item* first = nullptr;
        for (const auto* item : data.TodayStudy(clock.today))
            if (!data.marks.learned.count(item->id)) {
                ++left;
                if (!first)
                    first = item;
            }
        if (left) {
            r.active = true;
            r.board = kStudy;
            r.title = "今日学习 · " + std::to_string(left) + " 条没学";
            r.body = first->title;
            r.key = "study:" + clock.today;
            r.action = "P02";
            return r;
        }
    }

    // 5. Sunday night: a look back at the week.
    if (clock.weekday == 0 && clock.minutes >= 21 * 60 && offer("review:" + clock.today) &&
        !data.Review(clock.today).empty()) {
        r.active = true;
        r.board = kReview;
        r.title = "一周回顾";
        r.body = "看看这一周的运势、学习和待办";
        r.key = "review:" + clock.today;
        r.action = "A05";
    }
    return r;
}

}  // namespace tab5_nabo
