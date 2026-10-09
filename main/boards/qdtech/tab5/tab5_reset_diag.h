#pragma once

#include <string>

// Why did the Tab5 restart? A small RTC record (kept across panic, watchdog and brownout
// resets, lost only on a real power-off) tracks uptime, internal RAM and whether external
// audio was playing. After an abnormal reset the next boot stores the cause with that
// record in NVS, so it survives later resets (including the one a serial monitor causes).
namespace tab5_diag {

// Main task, early in boot (writes NVS).
void Start();
// e.g. "BROWNOUT at 216 s (audio on, internal free 41 KB, min 8 KB) · 3 times", or "".
std::string LastAbnormalReset();
// Call right before rebooting into the ota_0 app (updater / MD): the reset that brings
// the device back to factory is then logged but not counted as a fault.
void MarkLeavingForOtherApp();

}  // namespace tab5_diag
