#pragma once

#include <ctime>

// Offline "clinical pearls" for the home carousel: one ICU, one geriatrics and one blood-
// purification item per day, stepping through a fixed bank so every item comes back every
// N days (spaced repetition by rotation).  Source data: scripts/clinical_pearls.tsv.
struct Tab5Pearl {
    const char* title;   // short title, without the category prefix
    const char* body;    // <= 2 lines on the home card
    const char* source;  // guideline / trial name shown next to the item
};

struct Tab5PearlsToday {
    const char* prefix[3];  // "重症：" "老年：" "血净："
    Tab5Pearl item[3];
};

Tab5PearlsToday Tab5PearlsForDate(const tm& date);
