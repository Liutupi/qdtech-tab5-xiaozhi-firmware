#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class DesktopUI;

class RadioService {
public:
    using StateCallback = std::function<void(const char*, const char*, const char*)>;
    void Start(DesktopUI* desktop_ui, StateCallback callback = {});
    bool IsStarted() const { return started_.load(std::memory_order_acquire); }
    bool IsPlayRequested() const { return play_requested_.load(std::memory_order_acquire); }
    void Play();
    void Pause();
    void PlayPause();
    void Stop();
    void Next();
    void Prev();
    void SetPlaybackReleasedCallback(std::function<void()> callback);
    std::string PlayUrlFromTool(const std::string& title, const std::string& artist, const std::string& url);
    std::string GetStatusJson() const;
    std::string GetMusicStatusJson() const;
    uint32_t GetStreamGeneration() const {
        return stream_generation_.load(std::memory_order_acquire);
    }
    std::string SelectStation(const std::string& station);
    void SelectStationIndex(int index, int category_filter = -1);
    
    // 新增功能
    void ToggleFavorite(int index);
    bool IsFavorite(int index) const;
    std::vector<int> GetFavorites() const;
    std::vector<int> GetByCategory(int category) const;
    int GetCurrentIndex() const { return published_station_index_.load(std::memory_order_acquire); }
    int GetAudioLevel() const { return audio_level_.load(std::memory_order_relaxed); }
    int GetStationCount() const;
    const char* GetStationName(int index) const;
    const char* GetStationCategory(int index) const;
    int GetStationCategoryId(int index) const;

private:
    enum class Command {
        PLAY_PAUSE,
        PAUSE,
        STOP,
        NEXT,
        PREV,
        SELECT_STATION,
        FOCUS_CHANGED,
        PLAY_CUSTOM_URL,
    };
    struct CommandMessage {
        Command command;
        uint32_t sequence;
    };

    void PostCommand(Command command, bool user_control = true);
    void Task();
    void HandleCommand(Command command, uint32_t sequence = 0);
    void WaitForRetry(int delay_ms, uint32_t stream_generation);
    void PlayCurrentStation(uint32_t stream_generation);
    bool PlayUrl(const std::string& url, int url_index, uint32_t stream_generation);
    bool IsXiaozhiAudioState() const;
    bool IsCustomUrlSpeakingGraceActive() const;
    bool IsAutonomousCustomUrlSpeaking(int previous_state, int current_state) const;
    bool ShouldYieldAudio() const;
    void OnDeviceStateChanged(int previous_state, int current_state);
    void NextStation(int delta);
    int AdjacentCatalogIndex(int from, int delta, int category_filter) const;
    void SetUi(const char* state, const char* detail);
    bool FinishCustomUrlIfCurrent(uint32_t stream_generation, bool completed, const char* detail);
    void WritePcm(const int16_t* pcm, int samples, int channels, int sample_rate,
                  int16_t* mono_buffer, int mono_capacity,
                  int16_t* output_buffer, int output_capacity);
    void ResetAudioLeveler();
    void ApplyAudioLeveler(int16_t* pcm, int samples);
    void LoadFavorites();
    void SaveFavorites();
    void LoadStationIndex();
    void SaveStationIndex();
    void NotifyPlaybackReleased();
    void PublishCurrentStation();
    void ApplyPendingControl();
    void ReleaseExternalAudioIfNotReplacing();
    void ReleaseExternalAudioIfFocusBlocked();
    void SetExternalAudioActiveSerialized(bool active);

    DesktopUI* desktop_ui_ = nullptr;
    StateCallback state_callback_;
    std::mutex start_mutex_;
    void* queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    bool task_stack_internal_ = false;
    std::atomic<bool> started_{false};
    std::atomic<bool> play_requested_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> audio_focus_blocked_{false};
    std::atomic<bool> playback_release_pending_{false};
    // Set before interrupting a stream for another song/station. It keeps
    // external audio claimed until the player task consumes that replacement.
    std::atomic<bool> replacement_pending_{false};
    // Serializes radio-owned changes to Application's external audio state.
    // When both locks are needed, acquire this before submission_mutex_.
    std::mutex audio_focus_mutex_;
    std::mutex submission_mutex_;
    // User controls are serialized by submission_mutex_. A task-side command
    // acts only if it is still the newest submitted control.
    std::atomic<uint32_t> command_sequence_{0};
    // Coalesces rapid Play/Pause taps while stale queue entries are skipped.
    bool pending_toggle_parity_ = false;
    CommandMessage deferred_command_{Command::FOCUS_CHANGED, 0};
    bool deferred_command_valid_ = false;
    std::mutex playback_callback_mutex_;
    std::function<void()> playback_released_callback_;
    bool focus_pause_logged_ = false;
    int reconnect_attempt_ = 0;
    std::vector<int> last_success_url_;
    // Loaded before the player task starts. The final vector entry is a reserved
    // music URL slot; public station listings only expose catalog_station_count_.
    int catalog_station_count_ = 0;
    int station_index_ = 0;
    int custom_station_index_ = -1;
    int last_radio_station_index_ = 0;
    std::atomic<bool> playing_custom_url_{false};
    bool custom_url_stream_completed_ = false;
    bool custom_url_fatal_error_ = false;
    bool last_url_permanent_error_ = false;
    bool skip_reconnect_once_ = false;
    mutable std::mutex pending_mutex_;
    std::string pending_custom_name_;
    std::string pending_custom_url_;
    bool pending_custom_valid_ = false;
    int pending_station_index_ = -1;
    int pending_category_filter_ = -1;
    int pending_navigation_steps_ = 0;
    int active_category_filter_ = -1;
    std::atomic<TickType_t> custom_url_speaking_grace_until_{0};
    mutable std::mutex status_mutex_;
    std::string published_station_name_;
    std::string published_station_codec_;
    int published_station_bitrate_ = 0;
    std::atomic<int> published_station_index_{0};
    std::atomic<uint32_t> stream_generation_{0};
    std::atomic<int> audio_level_{0};
    // 0 stopped, 1 playing, 2 completed, 3 unavailable. Read by MCP from another task.
    std::atomic<int> music_playback_state_{0};
    int32_t audio_gain_q12_ = 4096;
};
