#pragma once

#include <ctime>

struct Tab5DailyContent {
    const char* quote = nullptr;
    const char* history_year = nullptr;
    const char* history_text = nullptr;
    const char* festival_title = nullptr;
    const char* festival_text = nullptr;
    const char* next_festival_title = nullptr;
    int days_to_next_festival = -1;
};

Tab5DailyContent Tab5DailyContentForDate(const tm& date);
