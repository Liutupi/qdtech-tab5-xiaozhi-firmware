#include "tab5_muse_inbox.h"
#include "tab5_podcast_transcript.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <memory>
#include <algorithm>

#include "board.h"
#include "settings.h"
#include "application.h"

#include <cstdlib>

namespace tab5_muse {
namespace {

const char* TAG = "Tab5Muse";
constexpr const char* kDefaultHost = "192.168.3.200:8787";
constexpr size_t kMaxJsonBytes = 32 * 1024;
constexpr size_t kMaxPodcastBytes = 192 * 1024;  // six bounded scripts and timed transcripts
constexpr int kPodcastLimit = 6;
constexpr int kTimeoutMs = 4000;
constexpr int kPollSeconds = 60;
constexpr int kMusicPollMs = 4000;
constexpr int kFirstPollDelayMs = 20000;  // let Wi-Fi and the XiaoZhi session settle first

std::string HttpGet(const std::string& url, int* status_out, size_t max_bytes = kMaxJsonBytes,
                    int timeout_ms = kTimeoutMs) {
    *status_out = 0;
    auto network = Board::GetInstance().GetNetwork();
    if (!network)
        return {};
    auto http = network->CreateHttp(0);
    if (!http)
        return {};
    http->SetTimeout(timeout_ms);
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", url))
        return {};
    const auto status = http->GetStatusCode();
    *status_out = status ? *status : 0;
    std::string body;
    if (*status_out == 200 && http->GetBodyLength() <= max_bytes) {
        char buffer[1024];
        while (body.size() <= max_bytes) {
            auto read = http->Read(buffer, sizeof(buffer));
            if (!read || *read == 0)
                break;
            if (body.size() + *read > max_bytes) {
                body.clear();
                break;
            }
            body.append(buffer, *read);
        }
    }
    http->Close();
    return body;
}

std::string JsonString(cJSON* object, const char* key) {
    auto* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// The poll task runs on a PSRAM stack, and NVS writes touch flash (cache disabled), which
// asserts on such a stack. Persist through the main task instead.
void SaveLater(const char* key, const std::string& value) {
    Application::GetInstance().Schedule([key = std::string(key), value] {
        Settings settings("museinbox", true);
        if (key == "seen")
            settings.SetInt(key, std::atoi(value.c_str()));
        else
            settings.SetString(key, value);
    });
}

}  // namespace

Inbox& Inbox::GetInstance() {
    static Inbox instance;
    return instance;
}

void Inbox::Start(Listener listener, MusicListener music_listener) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_)
            return;
        listener_ = std::move(listener);
        music_listener_ = std::move(music_listener);
        Settings settings("museinbox", false);
        snapshot_.host = settings.GetString("host", kDefaultHost);
        snapshot_.url = settings.GetString("url", "");
        topic_ = settings.GetString("topic", "");
        snapshot_.seen_id = settings.GetInt("seen", 0);
        last_music_id_ = settings.GetString("music_id", "");
    }
    // HTTP + cJSON parsing need a roomy stack; keep it out of internal SRAM.
    if (xTaskCreatePinnedToCoreWithCaps(TaskEntry, "muse_inbox", 8192, this, 2, &task_, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "task create failed");
    }
}

void Inbox::RequestRefresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refresh_requested_ = true;
    }
    if (task_)
        xTaskNotifyGive(task_);
}

void Inbox::ResolveTrack(int episode, int index, TrackCallback done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        track_pending_ = true;
        track_episode_ = episode;
        track_index_ = index;
        track_done_ = std::move(done);
    }
    RequestRefresh();
}

// Inbox task: the NAS looks the song up on NetEase (a few seconds), so allow a
// longer timeout than the regular polls.
void Inbox::ServeTrackRequest() {
    int episode = 0, index = 0;
    std::string url, host;
    TrackCallback done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!track_pending_)
            return;
        track_pending_ = false;
        episode = track_episode_;
        index = track_index_;
        done = std::move(track_done_);
        url = snapshot_.url;
        host = snapshot_.host;
    }
    std::string endpoint;
    if (url.empty()) {
        endpoint = "http://" + host + "/tab5/podcast/track";
    } else {
        const auto path = url.find("/inbox/");
        if (path == std::string::npos)
            return;
        endpoint = url;
        endpoint.replace(path, 7, "/podcast/");
        endpoint += "/track";
    }
    int status = 0;
    const std::string body =
        HttpGet(endpoint + "?episode=" + std::to_string(episode) + "&index=" + std::to_string(index),
                &status, 8 * 1024, 20000);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    ResolvedTrack track;
    track.episode = episode;
    track.index = index;
    if (cJSON_IsObject(root.get())) {
        auto* count = cJSON_GetObjectItem(root.get(), "count");
        track.count = cJSON_IsNumber(count) ? count->valueint : 0;
        track.title = JsonString(root.get(), "title");
        track.artist = JsonString(root.get(), "artist");
        track.url = JsonString(root.get(), "url");
        track.song_id = JsonString(root.get(), "song_id");
        track.ok = status == 200 && cJSON_IsTrue(cJSON_GetObjectItem(root.get(), "ok")) &&
                   (track.url.rfind("https://", 0) == 0 || track.url.rfind("http://", 0) == 0) &&
                   track.url.size() <= 1200 && track.song_id.size() <= 20;
    }
    ESP_LOGI(TAG, "podcast track %d/%d %s (http %d)", episode, index, track.ok ? "ok" : "unavailable",
             status);
    if (done)
        done(track);
}

void Inbox::MarkAllSeen() {
    Snapshot copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.seen_id == snapshot_.latest_id)
            return;
        snapshot_.seen_id = snapshot_.latest_id;
        Settings settings("museinbox", true);
        settings.SetInt("seen", snapshot_.seen_id);
        copy = snapshot_;
    }
    if (listener_)
        listener_(copy);
}

Snapshot Inbox::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void Inbox::SetHost(const std::string& host) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.host = host.empty() ? kDefaultHost : host;
        Settings settings("museinbox", true);
        settings.SetString("host", snapshot_.host);
    }
    RequestRefresh();
}

bool Inbox::SetUrl(const std::string& value) {
    std::string url = value;
    while (!url.empty() && (url.back() == '/' || url.back() == ' ' || url.back() == '\n'))
        url.pop_back();
    const auto mcp = url.find("/mcp/");
    if (mcp != std::string::npos)
        url.replace(mcp, 5, "/inbox/");
    if (url.rfind("https://", 0) != 0 || url.find("/inbox/") == std::string::npos)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.url = url;
        failures_ = 0;
        Settings settings("museinbox", true);
        settings.SetString("url", url);
    }
    ESP_LOGI(TAG, "inbox url set (%u chars)", unsigned(url.size()));
    RequestRefresh();
    return true;
}

// The relay publishes its current quick-tunnel hostname to an ntfy.sh topic it told us
// about earlier. Look it up and, when it changed, rewrite the stored URL.
bool Inbox::Discover() {
    std::string topic, url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        topic = topic_;
        url = snapshot_.url;
    }
    const auto path = url.find("/inbox/");
    if (topic.empty() || path == std::string::npos)
        return false;
    int status = 0;
    const std::string body =
        HttpGet("https://ntfy.sh/" + topic + "/json?poll=1&since=latest", &status);
    // Newline-delimited JSON; the last "message" event holds the hostname.
    std::string host;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos)
            end = body.size();
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> line(
            cJSON_ParseWithLength(body.data() + start, end - start), cJSON_Delete);
        if (line && JsonString(line.get(), "event") == "message")
            host = JsonString(line.get(), "message");
        start = end + 1;
    }
    if (host.empty() || host.find('/') != std::string::npos || host.size() > 120) {
        ESP_LOGW(TAG, "discovery: no host (http %d)", status);
        return false;
    }
    const std::string fresh = "https://" + host + url.substr(path);
    if (fresh == url)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.url = fresh;
        SaveLater("url", fresh);
    }
    ESP_LOGI(TAG, "discovery: relay moved to %s", host.c_str());
    return true;
}

void Inbox::TaskEntry(void* arg) { static_cast<Inbox*>(arg)->Run(); }

void Inbox::Run() {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFirstPollDelayMs));
    // The inbox/podcast poll runs on a wall-clock interval: each tunnel request
    // costs ~3 s of TLS setup, so counting music polls stretched it to ~5 min.
    int64_t last_poll_us = 0;
    bool refresh = true;
    while (true) {
        // A pending track lookup (user tapped play) goes first; it is not a reason
        // to re-poll the inbox.
        bool track_wake = false;
        bool requested = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            track_wake = track_pending_;
            requested = refresh_requested_;
            refresh_requested_ = false;
        }
        ServeTrackRequest();
        const int64_t now = esp_timer_get_time();
        if (requested || (refresh && !track_wake) ||
            now - last_poll_us >= int64_t(kPollSeconds) * 1000000) {
            Poll();
            last_poll_us = esp_timer_get_time();
        }
        PollMusic();
        refresh = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kMusicPollMs)) > 0;
    }
}

bool Inbox::PollMusic() {
    std::string host, url, last_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host = snapshot_.host;
        url = snapshot_.url;
        last_id = last_music_id_;
    }
    std::string endpoint;
    if (url.empty()) {
        endpoint = "http://" + host + "/tab5/music";
    } else {
        const auto path = url.find("/inbox/");
        if (path == std::string::npos)
            return false;
        endpoint = url;
        endpoint.replace(path, 7, "/music/");
    }
    int status = 0;
    const std::string body = HttpGet(endpoint, &status);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    if (status != 200 || !cJSON_IsObject(root.get())) {
        if (++music_failures_ == 2 && !url.empty())
            Discover();
        return false;
    }
    music_failures_ = 0;
    MusicCommand command;
    command.id = JsonString(root.get(), "id");
    if (command.id.empty() || command.id == last_id)
        return true;
    command.title = JsonString(root.get(), "title");
    command.artist = JsonString(root.get(), "artist");
    command.url = JsonString(root.get(), "url");
    command.song_id = JsonString(root.get(), "song_id");
    command.continuous = cJSON_IsTrue(cJSON_GetObjectItem(root.get(), "continuous"));
    if (command.id.size() > 64 || command.title.size() > 360 || command.artist.size() > 360 ||
        command.url.size() > 1200 ||
        (command.url.rfind("https://", 0) != 0 && command.url.rfind("http://", 0) != 0) ||
        command.song_id.size() > 20 ||
        !std::all_of(command.song_id.begin(), command.song_id.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        ESP_LOGW(TAG, "invalid music command ignored");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_music_id_ = command.id;
    }
    SaveLater("music_id", command.id);
    ESP_LOGI(TAG, "music command id=%s title=%s", command.id.c_str(), command.title.c_str());
    if (music_listener_)
        music_listener_(command);
    return true;
}

bool Inbox::FetchPodcasts(const std::string& endpoint, tab5_podcast::EpisodeList* out) {
    int status = 0;
    const std::string body = HttpGet(endpoint + "?limit=" + std::to_string(kPodcastLimit), &status,
                                     kMaxPodcastBytes);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    auto* list = root ? cJSON_GetObjectItem(root.get(), "episodes") : nullptr;
    if (status != 200 || !cJSON_IsArray(list)) {
        ESP_LOGW(TAG, "podcast fetch failed (http %d, %u bytes)", status, unsigned(body.size()));
        return false;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, list) {
        auto* id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsNumber(id) || out->size() >= size_t(kPodcastLimit))
            continue;
        tab5_podcast::Episode e;
        e.id = id->valueint;
        e.title = tab5_podcast::ToStr(JsonString(item, "title"));
        e.script = tab5_podcast::ToStr(JsonString(item, "script"));
        e.from = tab5_podcast::ToStr(JsonString(item, "from"));
        e.time = tab5_podcast::ToStr(JsonString(item, "time"));
        e.audio_url = tab5_podcast::ToStr(JsonString(item, "audio_url"));
        if (!e.audio_url.empty() && e.audio_url.rfind("https://", 0) != 0 &&
            e.audio_url.rfind("http://", 0) != 0)
            e.audio_url.clear();
        e.cues = tab5_podcast::ParseTranscript(item, e.audio_url);
        auto* tracks = cJSON_GetObjectItem(item, "tracks");
        cJSON* t = nullptr;
        cJSON_ArrayForEach (t, tracks) {
            if (e.tracks.size() >= tab5_podcast::kMaxTracks)
                break;
            tab5_podcast::Track track{tab5_podcast::ToStr(JsonString(t, "title")),
                                      tab5_podcast::ToStr(JsonString(t, "artist"))};
            if (!track.title.empty())
                e.tracks.push_back(std::move(track));
        }
        out->push_back(std::move(e));
    }
    return true;
}

bool Inbox::Poll() {
    std::string host, url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        host = snapshot_.host;
        url = snapshot_.url;
    }
    const std::string endpoint = url.empty() ? "http://" + host + "/tab5/inbox" : url;
    int status = 0;
    const std::string body = HttpGet(endpoint + "?since=0&limit=12", &status);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    auto* list = root ? cJSON_GetObjectItem(root.get(), "messages") : nullptr;
    const bool ok = cJSON_IsArray(list);

    // The MP3 is uploaded after the text publication, so audio_url can change
    // without a new episode id. Refresh each inbox poll, outside the lock.
    // podcast_id < 0: relay has no podcast support.
    auto* podcast_latest = ok ? cJSON_GetObjectItem(root.get(), "podcast_latest_id") : nullptr;
    const int podcast_id = cJSON_IsNumber(podcast_latest) ? podcast_latest->valueint : -1;
    const auto* latest = ok ? cJSON_GetObjectItem(root.get(), "latest_id") : nullptr;
    const int latest_id = cJSON_IsNumber(latest) ? latest->valueint : 0;
    std::string mcp_url = ok ? JsonString(root.get(), "mcp_url") : "";
    const std::string topic = ok ? JsonString(root.get(), "discovery_topic") : "";
    MessageList messages;
    if (ok) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach (item, list) {
            auto* id = cJSON_GetObjectItem(item, "id");
            if (!cJSON_IsNumber(id))
                continue;
            Message m;
            m.id = id->valueint;
            m.title = tab5_podcast::ToStr(JsonString(item, "title"));
            m.body = tab5_podcast::ToStr(JsonString(item, "body"));
            m.from = tab5_podcast::ToStr(JsonString(item, "from"));
            m.time = tab5_podcast::ToStr(JsonString(item, "time"));
            messages.push_back(std::move(m));
        }
    }
    // Do not keep the inbox JSON tree alive through another TLS handshake and
    // the much larger episode/transcript parse. Small cJSON allocations use SRAM.
    root.reset();
    auto fetched_episodes = tab5_podcast::MakeEpisodes();
    bool podcast_fetched = false;
    if (podcast_id >= 0) {
        std::string podcast_endpoint = endpoint;
        const auto path = podcast_endpoint.find("/inbox");
        if (path != std::string::npos)
            podcast_endpoint.replace(path, 6, "/podcast");
        podcast_fetched = FetchPodcasts(podcast_endpoint, fetched_episodes.get());
    }

    Snapshot copy;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) {
            changed = snapshot_.ok;
            snapshot_.ok = false;
            ++failures_;
            ESP_LOGW(TAG, "poll %s failed (http %d, %u bytes)", url.empty() ? host.c_str() : "tunnel",
                     status, unsigned(body.size()));
        } else {
            if (podcast_fetched) {
                snapshot_.podcast_latest_id = podcast_id;
                if (!snapshot_.episodes ||
                    !tab5_podcast::SameEpisodes(*snapshot_.episodes, *fetched_episodes)) {
                    size_t timed_cues = 0;
                    for (const auto& episode : *fetched_episodes)
                        timed_cues += episode.cues.size();
                    ESP_LOGI(TAG, "podcast latest=%d episodes=%u timed_cues=%u", podcast_id,
                             unsigned(fetched_episodes->size()), unsigned(timed_cues));
                    snapshot_.episodes = std::move(fetched_episodes);
                    changed = true;
                }
            } else if (podcast_id < 0) {
                // Relay without podcast support: derive episodes from daily-radio posts.
                int newest = 0;
                size_t count = 0;
                for (const auto& m : messages)
                    if (tab5_podcast::IsRadioPost(m.from, m.title)) {
                        newest = count++ ? newest : m.id;
                    }
                const size_t have = snapshot_.episodes ? snapshot_.episodes->size() : 0;
                if (newest != snapshot_.podcast_latest_id || count != have) {
                    auto episodes = tab5_podcast::MakeEpisodes();
                    for (const auto& m : messages) {
                        if (!tab5_podcast::IsRadioPost(m.from, m.title))
                            continue;
                        tab5_podcast::Episode e;
                        e.id = m.id;
                        e.title = tab5_podcast::ToStr(m.title);
                        e.script = tab5_podcast::ToStr(m.body);
                        e.from = tab5_podcast::ToStr(m.from);
                        e.time = tab5_podcast::ToStr(m.time);
                        e.tracks = tab5_podcast::ParseTracks(m.body);
                        episodes->push_back(std::move(e));
                    }
                    snapshot_.podcast_latest_id = newest;
                    snapshot_.episodes = std::move(episodes);
                    changed = true;
                }
            }
            changed = changed || !snapshot_.ok || latest_id != snapshot_.latest_id ||
                      messages.size() != snapshot_.messages.size() || mcp_url != snapshot_.mcp_url;
            if (latest_id < snapshot_.seen_id) {
                // The relay was reset (fresh data folder): start counting unread again.
                snapshot_.seen_id = 0;
                SaveLater("seen", "0");
            }
            failures_ = 0;
            if (!topic.empty() && topic != topic_) {
                topic_ = topic;
                SaveLater("topic", topic_);
            }
            snapshot_.ok = true;
            snapshot_.ever_ok = true;
            snapshot_.latest_id = latest_id;
            snapshot_.mcp_url = std::move(mcp_url);
            snapshot_.messages = std::move(messages);
            if (changed)
                ESP_LOGI(TAG, "inbox latest=%d unread=%d", latest_id, snapshot_.Unread());
        }
        snapshot_.podcast_ok = ok && (podcast_fetched || podcast_id < 0);
        ++snapshot_.poll_count;
        copy = snapshot_;
    }
    // Publish every completed poll so a manual refresh can report its result,
    // even when the episode contents have not changed. The UI defers unchanged lists.
    if (listener_)
        listener_(copy);
    // Two failures in a row through the tunnel: the relay may have a new hostname.
    if (!ok && !url.empty() && failures_ >= 2 && Discover())
        return Poll();
    return ok;
}

}  // namespace tab5_muse
