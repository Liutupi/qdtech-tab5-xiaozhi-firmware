#pragma once

#include <string>

namespace tab5_music_lyrics {

// Returns real timed LRC for a NetEase song ID or a matching title. When an
// artist is supplied, title-only matches from other artists are rejected.
// Must run on a worker task; playback may already have started.
std::string Lookup(const std::string& title, const std::string& artist, const std::string& song_id);

}  // namespace tab5_music_lyrics
