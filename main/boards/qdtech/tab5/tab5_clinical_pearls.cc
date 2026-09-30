#include "tab5_clinical_pearls.h"

#include <cstddef>

#include "tab5_clinical_pearls.inc"

namespace {

// Days since 1970-01-01 for a civil date (no time zone or mktime involved).
long DaysFromCivil(int year, int month, int day) {
    year -= month <= 2;
    const long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

template <size_t N>
const Tab5Pearl& Pick(const Tab5Pearl (&bank)[N], long day, long offset) {
    long index = (day + offset) % static_cast<long>(N);
    if (index < 0)
        index += static_cast<long>(N);
    return bank[index];
}

}  // namespace

Tab5PearlsToday Tab5PearlsForDate(const tm& date) {
    const long day = DaysFromCivil(date.tm_year + 1900, date.tm_mon + 1, date.tm_mday);
    Tab5PearlsToday today;
    today.prefix[0] = "重症：";
    today.prefix[1] = "老年：";
    today.prefix[2] = "血净：";
    today.item[0] = Pick(kPearlsIcu, day, 0);
    today.item[1] = Pick(kPearlsGer, day, 7);
    today.item[2] = Pick(kPearlsCrrt, day, 13);
    return today;
}
