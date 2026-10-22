#pragma once

#include <string>

#include "tab5_nabo_push.h"

// Home-screen reminder for the NABO 每日推送 boards: at most one at a time, picked by
// priority. The home daily card shows it and a tap opens that board; NABO plays one
// matching window-scene action. See docs/nabo-daily-push.md §3.
namespace tab5_nabo {

struct LocalClock {
    std::string today;  // YYYY-MM-DD, "" before the clock is set
    int minutes = 0;    // since local midnight
    int weekday = 0;    // 0 = Sunday
};

struct Reminder {
    bool active = false;
    int board = 0;  // Tab5NaboPushPage::Board
    std::string title, body;
    std::string key;             // one reminder instance; a tap dismisses it
    const char* action = "";     // lively-v1 clip id, e.g. "A07"
};

// Priority: to-do starting within 15 minutes (or up to an hour late), unread high-priority
// push, morning fortune (6:00-12:00, unread), evening study (20:00-23:00, not all learned),
// Sunday-night review (21:00 onwards). Dismissed keys are skipped.
Reminder PickReminder(const Data& data, const LocalClock& clock, const KeySet& dismissed);

// The window-scene clip for a pushed mood.
const char* MoodAction(const std::string& mood);

// No inserted animation in the night; the card still changes.
inline bool QuietHours(int minutes) { return minutes >= 23 * 60 || minutes < 7 * 60; }

}  // namespace tab5_nabo
