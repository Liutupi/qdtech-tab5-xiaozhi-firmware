#include <cassert>
#include <cstring>
#include <ctime>

#include "tab5_daily_content.h"

static tm Date(int year, int month, int day) {
    tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = 12;
    value.tm_isdst = -1;
    mktime(&value);
    return value;
}

int main() {
    const auto september = Tab5DailyContentForDate(Date(2026, 9, 27));
    assert(september.quote && *september.quote);
    assert(september.history_year && std::strcmp(september.history_year, "2007") == 0);
    assert(september.history_text && *september.history_text);
    assert(std::strcmp(september.next_festival_title, "国庆节") == 0);
    assert(september.days_to_next_festival == 4);

    const auto mid_autumn = Tab5DailyContentForDate(Date(2026, 9, 25));
    assert(std::strcmp(mid_autumn.festival_title, "中秋节") == 0);
    assert(mid_autumn.days_to_next_festival == 0);

    const auto new_year = Tab5DailyContentForDate(Date(2026, 12, 31));
    assert(std::strcmp(new_year.festival_title, "跨年夜") == 0);
    assert(new_year.days_to_next_festival == 0);
    return 0;
}
