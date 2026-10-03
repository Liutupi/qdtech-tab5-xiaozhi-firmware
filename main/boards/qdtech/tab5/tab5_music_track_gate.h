#pragma once

#include <cstdint>
#include <string>

namespace tab5_music_track_gate {

// Access is serialized by the board's music_track_mutex_. A legacy lyric call
// has no track token, so it must identify the track by its title and, when
// available, artist. Consecutive songs with the same title require an artist.
class CurrentTrack {
public:
    void Begin(uint32_t generation, const std::string& title, const std::string& artist) {
        title_reused_ = active_ && title_ == title;
        generation_ = generation;
        title_ = title;
        artist_ = artist;
        active_ = true;
    }

    bool IsCurrent(uint32_t generation) const { return active_ && generation_ == generation; }

    bool Accepts(uint32_t generation, const std::string& title, const std::string& artist) const {
        if (!IsCurrent(generation) || title.empty() || title != title_)
            return false;
        if (!artist.empty())
            return artist == artist_;
        return !title_reused_;
    }

    void InvalidateIfCurrent(uint32_t generation) {
        if (IsCurrent(generation))
            active_ = false;
    }

private:
    uint32_t generation_ = 0;
    std::string title_;
    std::string artist_;
    bool active_ = false;
    bool title_reused_ = false;
};

}  // namespace tab5_music_track_gate
