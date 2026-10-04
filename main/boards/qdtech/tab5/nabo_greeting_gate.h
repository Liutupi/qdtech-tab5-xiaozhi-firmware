#pragma once

#include <cstdint>

// Host-tested presence greeting policy. Greets someone who actually stays in
// front of the device, once per visit; passers-by and brief detector misses of a
// seated person never trigger it. Times are microseconds (esp_timer clock).
namespace nabo_vision {
class GreetingGate {
public:
    static constexpr int64_t kVisitGapUs = 4LL * 1000000;   // longer gap = new visit
    static constexpr int64_t kDwellUs = 6LL * 1000000;      // must stay this long
    static constexpr unsigned kMinHits = 4;                 // detections within the dwell
    static constexpr int64_t kCooldownUs = 10LL * 60 * 1000000;
    static constexpr int64_t kReturnAbsenceUs = 5LL * 60 * 1000000;

    // Call after each person-detector scan. Returns true exactly once for a visit
    // that qualifies for a greeting. last_greeting_us: 0 if never greeted.
    bool Scan(int64_t now, bool person, int64_t last_greeting_us) {
        if (!person)
            return false;
        if (!visit_start_ || now - last_seen_ > kVisitGapUs) {
            absence_ = last_seen_ ? now - last_seen_ : INT64_MAX;
            visit_start_ = now;
            hits_ = 0;
            considered_ = false;
        }
        last_seen_ = now;
        ++hits_;
        if (considered_ || now - visit_start_ < kDwellUs || hits_ < kMinHits)
            return false;
        considered_ = true;
        if (!last_greeting_us)
            return true;
        return now - last_greeting_us >= kCooldownUs && absence_ >= kReturnAbsenceUs;
    }
    // True while a visit is still being evaluated; scan faster to measure dwell.
    bool Evaluating(int64_t now) const {
        return visit_start_ && !considered_ && now - last_seen_ <= kVisitGapUs;
    }

private:
    int64_t visit_start_ = 0, last_seen_ = 0, absence_ = 0;
    unsigned hits_ = 0;
    bool considered_ = false;
};
}  // namespace nabo_vision
