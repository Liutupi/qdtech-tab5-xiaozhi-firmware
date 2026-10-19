#include "tab5_song_favorites.h"
#include <sys/stat.h>
#include <new>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tab5_sd.h"

namespace {
constexpr const char* kFile = "/sdcard/tab5/nabo/favorite-songs.bin";
constexpr const char* kTag = "SongFavorites";
}  // namespace
Tab5SongFavorites* Tab5SongFavorites::Get() {
    static Tab5SongFavorites* instance = []() -> Tab5SongFavorites* {
        constexpr auto caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        void* memory = heap_caps_malloc(sizeof(Tab5SongFavorites), caps);
        if (!memory)
            return nullptr;
        auto* self = new (memory) Tab5SongFavorites;
        self->state_.songs = nabo_song_favorites::MakeList();
        if (xTaskCreatePinnedToCoreWithCaps(
                [](void* arg) { static_cast<Tab5SongFavorites*>(arg)->Run(); }, "song_favorites",
                6144, self, 2, &self->task_, 1, caps) != pdPASS) {
            self->~Tab5SongFavorites();
            heap_caps_free(memory);
            return nullptr;
        }
        return self;
    }();
    return instance;
}
nabo_song_favorites::Snapshot Tab5SongFavorites::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
nabo_song_favorites::Change Tab5SongFavorites::Toggle(const nabo_song_favorites::Track& song) {
    using namespace nabo_song_favorites;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.ready || !Valid(song))
        return Change::Unavailable;
    auto next = MakeList();
    *next = *state_.songs;
    const auto found = std::find_if(next->begin(), next->end(),
                                    [&](const auto& item) { return Same(item, song); });
    const bool remove = found != next->end();
    if (remove)
        next->erase(found);
    else {
        if (next->size() >= kMaxSongs)
            return Change::Full;
        auto saved = song;
        const auto known = std::find_if(known_.begin(), known_.end(),
                                        [&](const auto& item) { return Same(item, song); });
        if (saved.song_id.empty() && known != known_.end())
            saved.song_id = known->song_id;
        next->insert(next->begin(), std::move(saved));
    }
    state_.songs = std::move(next);
    state_.pending = true;
    state_.error = Error::None;
    ++state_.revision;
    ++desired_revision_;
    xTaskNotifyGive(task_);
    return remove ? Change::Removed : Change::Added;
}
void Tab5SongFavorites::Remember(const std::string& title, const std::string& artist,
                                 const std::string& id) {
    using namespace nabo_song_favorites;
    Track song{tab5_podcast::ToStr(title), tab5_podcast::ToStr(artist), tab5_podcast::ToStr(id)};
    if (id.empty() || !Valid(song))
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto known = std::find_if(known_.begin(), known_.end(),
                              [&](const auto& item) { return Same(item, song); });
    if (known != known_.end())
        known_.erase(known);
    known_.insert(known_.begin(), song);
    if (known_.size() > kMaxSongs)
        known_.pop_back();
    if (!state_.ready)
        return;
    auto saved = std::find_if(state_.songs->begin(), state_.songs->end(),
                              [&](const auto& item) { return Same(item, song); });
    if (saved == state_.songs->end() || saved->song_id == song.song_id)
        return;
    auto next = MakeList();
    *next = *state_.songs;
    (*next)[size_t(saved - state_.songs->begin())].song_id = song.song_id;
    state_.songs = std::move(next);
    state_.pending = true;
    state_.error = Error::None;
    ++state_.revision;
    ++desired_revision_;
    xTaskNotifyGive(task_);
}
void Tab5SongFavorites::Run() {
    using namespace nabo_song_favorites;
    while (!Tab5SdReady())
        vTaskDelay(pdMS_TO_TICKS(500));
    auto loaded = MakeList();
    const bool ok = LoadFile(kFile, *loaded);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.songs = loaded;
        persisted_ = loaded;
        state_.ready = ok;
        state_.error = ok ? Error::None : Error::Corrupt;
        ++state_.revision;
    }
    ESP_LOGI(kTag, "load %s songs=%u", ok ? "ok" : "failed", unsigned(loaded->size()));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        SharedList songs;
        uint32_t revision;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!state_.pending)
                continue;
            songs = state_.songs;
            revision = desired_revision_;
        }
        // Write a complete immutable snapshot; a burst of taps coalesces to the
        // newest desired revision. No lock is held while accessing the card.
        mkdir("/sdcard/tab5", 0775);
        mkdir("/sdcard/tab5/nabo", 0775);
        const bool saved = SaveFile(kFile, *songs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (saved)
                persisted_ = songs;
            if (revision == desired_revision_) {
                state_.pending = false;
                if (!saved) {
                    state_.songs = persisted_;
                    state_.error = Error::Write;
                }
                ++state_.revision;
            }
        }
        ESP_LOGI(kTag, "save %s songs=%u", saved ? "ok" : "failed", unsigned(songs->size()));
    }
}
