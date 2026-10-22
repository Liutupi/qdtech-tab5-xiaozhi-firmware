#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "tab5_podcast_model.h"

// Polls the NAS "Muse inbox" relay over LAN or its public tunnel. Muse pushes short
// messages to the relay through a public MCP endpoint; music commands share the relay.
namespace tab5_muse {

struct Message {
    int id = 0;
    std::string title;
    std::string body;
    std::string from;
    std::string time;
};

struct MusicCommand {
    std::string id;
    std::string title;
    std::string artist;
    std::string url;
    std::string song_id;
    bool continuous = false;
};

// One Muse 电台 track resolved by the NAS (full-length NetEase URL).
struct ResolvedTrack {
    bool ok = false;
    bool is_trial = false;
    int http_status = 0;
    std::string error_code;
    int episode = 0;
    int index = 0;
    int count = 0;
    std::string title;
    std::string artist;
    std::string url;
    std::string song_id;
};

struct RelayConnection {
    std::string url;
    std::string server_name;
};
// Reuse a recently TLS-verified relay address for narrated audio. This only takes
// a short cache lock; it never resolves DNS or waits for an inbox HTTP transfer.
RelayConnection CachedRelayConnection(const std::string& url);

struct Snapshot {
    bool ok = false;          // last poll succeeded
    bool ever_ok = false;     // at least one poll succeeded since boot
    int latest_id = 0;
    int seen_id = 0;          // highest id the user has viewed
    std::string host;         // NAS host:port (LAN) — used only when no url is set
    std::string url;          // public inbox URL https://<tunnel>/inbox/<token>
    std::string mcp_url;      // public MCP URL (only when the tunnel is up)
    std::vector<Message> messages;  // newest first
    uint32_t poll_count = 0;        // completed inbox/episode fetches, including failures
    bool podcast_ok = false;
    int podcast_latest_id = 0;      // relay's newest music-radio episode id
    tab5_podcast::SharedEpisodes episodes;  // newest first, shared (PSRAM), may be null

    int Unread() const {
        int n = 0;
        for (const auto& m : messages)
            n += m.id > seen_id;
        return n;
    }
};

class Inbox {
public:
    using Listener = std::function<void(const Snapshot&)>;
    using MusicListener = std::function<void(const MusicCommand&)>;
    using TrackCallback = std::function<void(const ResolvedTrack&)>;

    static Inbox& GetInstance();
    void Start(Listener listener, MusicListener music_listener = {});
    void RequestRefresh();
    // Asks the relay to resolve one episode track; done runs on the inbox task.
    // A newer request replaces a pending one.
    void ResolveTrack(int episode, int index, TrackCallback done);
    void ResolveSong(const std::string& title, const std::string& artist,
                     const std::string& song_id, TrackCallback done);
    void MarkAllSeen();
    Snapshot Current();
    void SetHost(const std::string& host);
    // Accepts the Muse MCP URL (.../mcp/<token>) or the inbox URL (.../inbox/<token>).
    bool SetUrl(const std::string& url);

private:
    Inbox() = default;
    static void TaskEntry(void* arg);
    void Run();
    bool Poll();
    bool PollMusic();
    void ServeTrackRequest();
    bool FetchPodcasts(const std::string& endpoint, tab5_podcast::EpisodeList* out);
    // NABO 每日推送 (/nabo/<token>/inbox beside the tunnel inbox) into tab5_nabo::Store.
    void PollNabo(const std::string& url);
    bool Discover();
    int failures_ = 0;
    std::string topic_;
    std::string last_music_id_;
    int music_failures_ = 0;
    bool refresh_requested_ = false;
    bool track_pending_ = false;
    int track_episode_ = 0;
    int track_index_ = 0;
    std::string track_title_, track_artist_, track_song_id_;
    TrackCallback track_done_;

    std::mutex mutex_;
    Snapshot snapshot_;
    Listener listener_;
    MusicListener music_listener_;
    TaskHandle_t task_ = nullptr;
};

}  // namespace tab5_muse
