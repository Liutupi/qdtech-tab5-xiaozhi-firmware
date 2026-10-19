#pragma once
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nabo_song_favorites.h"

// SD-only worker. UI calls copy immutable state and never perform filesystem IO.
class Tab5SongFavorites {
public:
    static Tab5SongFavorites* Get();
    nabo_song_favorites::Snapshot Current();
    // Remember /track identities even before a song is favorited. Bounded, PSRAM only.
    void Remember(const std::string& title, const std::string& artist, const std::string& id);
    nabo_song_favorites::Change Toggle(const nabo_song_favorites::Track& song);

private:
    void Run();
    std::mutex mutex_;
    nabo_song_favorites::Snapshot state_;
    nabo_song_favorites::SharedList persisted_;
    uint32_t desired_revision_ = 0;
    nabo_song_favorites::List known_;
    TaskHandle_t task_ = nullptr;
};
