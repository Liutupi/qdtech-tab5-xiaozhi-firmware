#pragma once

#include <cstring>

#include "cJSON.h"
#include "tab5_podcast_model.h"

namespace tab5_podcast {

// Reject an entire malformed or stale timeline. Never estimate sentence times.
inline CueList ParseTranscript(const cJSON* episode, std::string_view audio_url) {
    CueList result;
    const auto* transcript = cJSON_GetObjectItemCaseSensitive(episode, "transcript");
    const auto* filename = cJSON_GetObjectItemCaseSensitive(transcript, "audio_filename");
    const auto* cues = cJSON_GetObjectItemCaseSensitive(transcript, "cues");
    const size_t slash = audio_url.find_last_of('/');
    if (!cJSON_IsString(filename) || !filename->valuestring || slash == std::string_view::npos ||
        audio_url.substr(slash + 1) != filename->valuestring || !cJSON_IsArray(cues))
        return result;
    const int count = cJSON_GetArraySize(cues);
    if (count <= 0 || count > int(kMaxCues))
        return result;
    result.reserve(count);
    size_t bytes = 0;
    uint32_t previous_end = 0;
    const cJSON* cue = nullptr;
    cJSON_ArrayForEach (cue, cues) {
        const auto* start = cJSON_GetObjectItemCaseSensitive(cue, "start_ms");
        const auto* end = cJSON_GetObjectItemCaseSensitive(cue, "end_ms");
        const auto* text = cJSON_GetObjectItemCaseSensitive(cue, "text");
        const auto* kind = cJSON_GetObjectItemCaseSensitive(cue, "kind");
        if (!cJSON_IsNumber(start) || !cJSON_IsNumber(end) ||
            !(start->valuedouble >= previous_end && start->valuedouble < end->valuedouble &&
              end->valuedouble <= kMaxCueTimeMs) ||
            start->valuedouble != start->valueint || end->valuedouble != end->valueint ||
            !cJSON_IsString(text) || !text->valuestring)
            return {};
        const size_t size = std::strlen(text->valuestring);
        bytes += size;
        if (!size || size > 1536 || bytes > kMaxCueTextBytes || TrimView(text->valuestring).empty())
            return {};
        previous_end = end->valueint;
        const bool music = cJSON_IsString(kind) && kind->valuestring &&
                           std::strcmp(kind->valuestring, "music") == 0;
        result.push_back(
            {uint32_t(start->valueint), previous_end, ToStr(text->valuestring), music});
    }
    return result;
}

}  // namespace tab5_podcast
