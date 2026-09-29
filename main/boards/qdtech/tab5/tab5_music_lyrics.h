#pragma once

#include <string>

namespace tab5_music_lyrics {

// Returns real timed LRC for an exact NetEase title/artist match, or empty.
// Must run on a worker task before opening the song stream.
std::string Lookup(const std::string& title, const std::string& artist, const std::string& song_id);

}  // namespace tab5_music_lyrics
