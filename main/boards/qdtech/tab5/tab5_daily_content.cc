#include "tab5_daily_content.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>

struct FestivalEntry {
    uint8_t month;
    uint8_t day;
    const char* title;
    const char* text;
};

struct DatedFestivalEntry {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    const char* title;
    const char* text;
};

struct HistoryEntry {
    uint8_t month;
    uint8_t day;
    const char* year;
    const char* text;
};

static const FestivalEntry FESTIVALS[] = {
    {1, 1, "元旦", "新的一年，愿你日日有光。"},
    {2, 14, "情人节", "愿爱与被爱，都温柔以待。"},
    {3, 8, "妇女节", "愿她们自在生长，温柔而有力量。"},
    {3, 12, "植树节", "种下一点绿色，也种下一点希望。"},
    {4, 1, "愚人节", "生活需要一点轻松的玩笑。"},
    {5, 1, "劳动节", "向认真生活的人致敬。"},
    {5, 4, "青年节", "愿热爱不老，脚步不停。"},
    {5, 12, "护士节", "白衣执甲，温柔守护。"},
    {6, 1, "儿童节", "愿童心常在，眼里有光。"},
    {6, 7, "高考日", "十二年磨一剑，愿你从容落笔。"},
    {7, 1, "建党节", "初心如磐，步履向前。"},
    {8, 1, "建军节", "山河有守，万家灯火。"},
    {9, 10, "教师节", "一支粉笔，点亮许多远方。"},
    {10, 1, "国庆节", "山河锦绣，家国同庆。"},
    {10, 31, "万圣节", "今夜扮鬼，明天继续可爱。"},
    {11, 11, "双十一", "理性消费，快乐购物。"},
    {12, 24, "平安夜", "愿今夜平安，岁岁年年。"},
    {12, 25, "圣诞节", "愿冬夜有暖，心中有光。"},
    {12, 31, "跨年夜", "旧岁已展千重锦，新年再进百尺竿。"},
};

static const DatedFestivalEntry LUNAR_FESTIVALS[] = {
    {2024, 2, 10, "春节", "新春开岁，愿你万事顺意。"},
    {2024, 2, 24, "元宵节", "灯火映团圆，心里有暖光。"},
    {2024, 6, 10, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2024, 8, 10, "七夕", "星河有约，温柔常在。"},
    {2024, 9, 17, "中秋节", "月满人团圆，清辉照归途。"},
    {2024, 10, 11, "重阳节", "登高望远，岁岁安康。"},
    {2025, 1, 7, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2025, 1, 28, "除夕", "旧岁至此团圆，新年就在门前。"},
    {2025, 1, 29, "春节", "新春开岁，愿你万事顺意。"},
    {2025, 2, 12, "元宵节", "灯火映团圆，心里有暖光。"},
    {2025, 5, 31, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2025, 8, 29, "七夕", "星河有约，温柔常在。"},
    {2025, 10, 6, "中秋节", "月满人团圆，清辉照归途。"},
    {2025, 10, 29, "重阳节", "登高望远，岁岁安康。"},
    {2026, 1, 26, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2026, 2, 16, "除夕", "旧岁至此团圆，新年就在门前。"},
    {2026, 2, 17, "春节", "新春开岁，愿你万事顺意。"},
    {2026, 3, 3, "元宵节", "灯火映团圆，心里有暖光。"},
    {2026, 6, 19, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2026, 8, 19, "七夕", "星河有约，温柔常在。"},
    {2026, 9, 25, "中秋节", "月满人团圆，清辉照归途。"},
    {2026, 10, 18, "重阳节", "登高望远，岁岁安康。"},
    {2027, 1, 15, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2027, 2, 5, "除夕", "旧岁至此团圆，新年就在门前。"},
    {2027, 2, 6, "春节", "新春开岁，愿你万事顺意。"},
    {2027, 2, 20, "元宵节", "灯火映团圆，心里有暖光。"},
    {2027, 6, 9, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2027, 8, 8, "七夕", "星河有约，温柔常在。"},
    {2027, 9, 15, "中秋节", "月满人团圆，清辉照归途。"},
    {2027, 10, 8, "重阳节", "登高望远，岁岁安康。"},
    {2028, 1, 4, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2028, 1, 25, "除夕", "旧岁至此团圆，新年就在门前。"},
    {2028, 1, 26, "春节", "新春开岁，愿你万事顺意。"},
    {2028, 2, 9, "元宵节", "灯火映团圆，心里有暖光。"},
    {2028, 5, 28, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2028, 8, 26, "七夕", "星河有约，温柔常在。"},
    {2028, 10, 3, "中秋节", "月满人团圆，清辉照归途。"},
    {2028, 10, 26, "重阳节", "登高望远，岁岁安康。"},
    {2029, 1, 22, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2029, 2, 12, "除夕", "旧岁至此团圆，新年就在门前。"},
    {2029, 2, 13, "春节", "新春开岁，愿你万事顺意。"},
    {2029, 2, 27, "元宵节", "灯火映团圆，心里有暖光。"},
    {2029, 6, 16, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2029, 8, 16, "七夕", "星河有约，温柔常在。"},
    {2029, 9, 22, "中秋节", "月满人团圆，清辉照归途。"},
    {2029, 10, 16, "重阳节", "登高望远，岁岁安康。"},
    {2030, 1, 11, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2030, 2, 2, "除夕", "旧岁至此团圆，新年就在门前。"},
    {2030, 2, 3, "春节", "新春开岁，愿你万事顺意。"},
    {2030, 2, 17, "元宵节", "灯火映团圆，心里有暖光。"},
    {2030, 6, 5, "端午节", "粽叶飘香，愿你安康顺遂。"},
    {2030, 8, 5, "七夕", "星河有约，温柔常在。"},
    {2030, 9, 12, "中秋节", "月满人团圆，清辉照归途。"},
    {2030, 10, 5, "重阳节", "登高望远，岁岁安康。"},
    {2031, 1, 1, "腊八节", "一碗腊八粥，暖过岁寒时。"},
    {2031, 1, 22, "除夕", "旧岁至此团圆，新年就在门前。"},
};

static const HistoryEntry HISTORY_TODAY[] = {
    {1, 1, "1912", "中华民国临时政府成立。"},
    {1, 8, "1942", "斯蒂芬·霍金出生。"},
    {1, 15, "2001", "维基百科上线。"},
    {2, 14, "1876", "贝尔申请电话专利。"},
    {2, 19, "1997", "邓小平逝世。"},
    {3, 8, "1910", "国际妇女节首次庆祝。"},
    {3, 12, "1925", "孙中山先生逝世。"},
    {3, 14, "1879", "爱因斯坦出生。"},
    {4, 12, "1961", "加加林进入太空。"},
    {4, 22, "1970", "第一个地球日。"},
    {5, 1, "1886", "芝加哥工人大罢工。"},
    {5, 4, "1919", "五四运动爆发。"},
    {5, 12, "1820", "南丁格尔出生。"},
    {6, 1, "1925", "上海儿童幸福节。"},
    {6, 5, "1972", "联合国人类环境会议开幕。"},
    {6, 15, "1215", "英国《大宪章》签署。"},
    {7, 1, "1997", "香港回归中国。"},
    {7, 4, "1776", "美国独立宣言发表。"},
    {7, 20, "1969", "人类首次登上月球。"},
    {8, 15, "1945", "日本宣布无条件投降。"},
    {9, 1, "1983", "苏联击落韩国客机。"},
    {9, 10, "1985", "中国第一个教师节。"},
    {9, 11, "2001", "美国911事件。"},
    // NASA mission timeline: https://science.nasa.gov/mission/dawn/
    {9, 27, "2007", "NASA Dawn 探测器起航。"},
    {10, 1, "1949", "中华人民共和国成立。"},
    {10, 24, "1945", "联合国成立。"},
    {11, 11, "1918", "第一次世界大战结束。"},
    {11, 12, "1866", "孙中山出生。"},
    {12, 12, "1936", "西安事变。"},
    {12, 13, "1937", "南京大屠杀纪念日。"},
    {12, 20, "1999", "澳门回归中国。"},
    {12, 26, "1893", "毛泽东出生。"},
};

static const char* DAILY_QUOTES[] = {
    "把眼前的小事做好，日子自然会发光。",
    "慢慢来，也是在向前。",
    "今天也要认真吃饭，认真生活。",
    "心里有光，路就不算远。",
    "先完成，再慢慢变好。",
    "把复杂留给系统，把温柔留给自己。",
    "愿你有耐心，也有锋芒。",
    "安静努力的人，终会被时间看见。",
    "每一次稳定，都是未来的底座。",
    "不必追赶所有风，只要守住方向。",
    "把今天过清楚，就是很好的答案。",
    "小步不停，也会走到远方。",
    "愿你的热爱，有处安放。",
    "清醒一点，松弛一点，继续一点。",
    "生活会奖励认真修补的人。",
    "今天的好心情，从少一点杂音开始。",
    "日拱一卒，功不唐捐。",
    "保持热爱，奔赴山海。",
    "你只管努力，剩下的交给时间。",
    "简单的事重复做，你就是专家。",
    "每天进步一点点，就是最好的状态。",
    "把期待降到最低，把热情提到最高。",
    "生活不是等待暴风雨过去，而是学会在雨中跳舞。",
    "你现在的努力，是未来的底气。",
    "别着急，最好的总会在最不经意的时候出现。",
    "做自己生命的主角，而非别人生命的看客。",
    "温柔半两，从容一生。",
    "愿你眼中有光，心中有爱，脚下有路。",
    "今天的你，比昨天更值得被爱。",
    "生活明朗，万物可爱。",
    "慢慢变好，就是给自己最好的礼物。",
    "愿所有的后会有期，都是他日的别来无恙。",
};

static const FestivalEntry* FindFestival(int month, int day) {
    for (const auto& entry : FESTIVALS) {
        if (entry.month == month && entry.day == day) {
            return &entry;
        }
    }
    return nullptr;
}

static const DatedFestivalEntry* FindLunarFestival(int year, int month, int day) {
    for (const auto& entry : LUNAR_FESTIVALS) {
        if (entry.year == year && entry.month == month && entry.day == day) {
            return &entry;
        }
    }
    return nullptr;
}

static const HistoryEntry* FindHistoryToday(int month, int day) {
    for (const auto& entry : HISTORY_TODAY) {
        if (entry.month == month && entry.day == day) {
            return &entry;
        }
    }
    return nullptr;
}

Tab5DailyContent Tab5DailyContentForDate(const tm& date) {
    Tab5DailyContent content;
    const int year = date.tm_year + 1900;
    const int month = date.tm_mon + 1;
    const int day = date.tm_mday;
    const size_t quote_count = sizeof(DAILY_QUOTES) / sizeof(DAILY_QUOTES[0]);
    content.quote = DAILY_QUOTES[date.tm_yday % quote_count];
    if (const auto* history = FindHistoryToday(month, day)) {
        content.history_year = history->year;
        content.history_text = history->text;
    }
    if (const auto* festival = FindLunarFestival(year, month, day)) {
        content.festival_title = festival->title;
        content.festival_text = festival->text;
    } else if (const auto* festival = FindFestival(month, day)) {
        content.festival_title = festival->title;
        content.festival_text = festival->text;
    }

    tm today = date;
    today.tm_hour = 12;
    today.tm_min = today.tm_sec = 0;
    today.tm_isdst = -1;
    const time_t today_time = mktime(&today);
    auto consider = [&](int target_year, int target_month, int target_day, const char* title) {
        tm candidate = today;
        candidate.tm_year = target_year - 1900;
        candidate.tm_mon = target_month - 1;
        candidate.tm_mday = target_day;
        candidate.tm_isdst = -1;
        const time_t target_time = mktime(&candidate);
        const int days =
            static_cast<int>(std::lround(std::difftime(target_time, today_time) / 86400));
        if (days >= 0 &&
            (content.days_to_next_festival < 0 || days < content.days_to_next_festival)) {
            content.days_to_next_festival = days;
            content.next_festival_title = title;
        }
    };
    for (const auto& festival : FESTIVALS) {
        consider(year, festival.month, festival.day, festival.title);
        consider(year + 1, festival.month, festival.day, festival.title);
    }
    for (const auto& festival : LUNAR_FESTIVALS) {
        if (festival.year == year || festival.year == year + 1)
            consider(festival.year, festival.month, festival.day, festival.title);
    }
    return content;
}
