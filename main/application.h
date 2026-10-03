#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "notify/notify_player.h"
#include "background_task.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    bool AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    // Send a spoken-style text command to the server (no wake word audio).
    // Returns false when the device is busy (not idle) and nothing was sent.
    bool InvokeTextCommand(const std::string& text);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "",
                         const std::string& expected_sha256 = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    BackgroundTask* GetBackgroundTask() { return background_task_.get(); }
    void SetExternalAudioActive(bool active);
    bool IsExternalAudioActive() const { return external_audio_active_.load(); }
    void PrepareExternalAudioPlayback();
    void RegisterDeviceStateCallback(std::function<void(DeviceState, DeviceState)> callback);
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();
    // Safe from protocol/network callbacks; closes on the protocol worker.
    void CloseAudioChannelAsync(bool send_goodbye = true);
    // Used by transport callbacks with a session check; the predicate runs on
    // the protocol worker immediately before teardown.
    void CloseAudioChannelAsync(bool send_goodbye, std::function<bool(Protocol&)> still_current);
    // MQTT's idle reconnect uses the same worker and access gate as channel open.
    void ScheduleProtocolMaintenance(std::function<void(Protocol&)> operation);

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    // The main task owns the published protocol; background operations retain
    // a snapshot so a Wi-Fi reset cannot destroy an in-flight handshake.
    std::shared_ptr<Protocol> protocol_;
    mutable std::mutex protocol_mutex_;
    std::mutex protocol_io_mutex_;
    std::atomic<uint64_t> protocol_epoch_{0};
    std::atomic<uint64_t> pending_protocol_error_epoch_{0};
    std::atomic<unsigned> protocol_work_pending_{0};
    std::atomic<bool> suppress_close_callback_{false};
    bool protocol_open_queued_ = false;               // guarded by protocol_mutex_
    uint64_t protocol_open_epoch_ = 0;                // guarded by protocol_mutex_
    uint64_t protocol_closed_during_open_epoch_ = 0;  // guarded by protocol_mutex_
    std::function<void()> protocol_open_completion_;  // guarded by protocol_mutex_
    struct PendingOpenCompletion {
        std::weak_ptr<Protocol> identity;
        uint64_t epoch;
        bool opened;
    };
    std::optional<PendingOpenCompletion> pending_open_completion_;  // main task only
    bool protocol_maintenance_queued_ = false;                      // guarded by protocol_mutex_
    uint64_t protocol_maintenance_epoch_ = 0;                       // guarded by protocol_mutex_
    std::mutex protocol_reset_mutex_;
    std::condition_variable protocol_reset_cv_;
    unsigned protocol_resets_pending_ = 0;
    // Accessed only by the main task. Responses arriving during a handshake
    // are kept briefly, then sent only if that same channel opens.
    std::deque<std::pair<uint64_t, std::string>> pending_mcp_messages_;
    struct PendingProtocolAction {
        std::weak_ptr<Protocol> identity;
        uint64_t epoch;
        std::string session_id;
        std::function<void()> action;
    };
    std::deque<PendingProtocolAction> pending_protocol_actions_;  // main task only
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::atomic<bool> external_audio_active_{false};
    std::unique_ptr<BackgroundTask> background_task_;
    // The Tab5 sends voice/control traffic from a dedicated worker. A slow
    // socket must not stall the main event loop or the audio codec task.
    std::unique_ptr<BackgroundTask> outbound_task_;
    enum class OutboundKind { Audio, WakeWord, Start, Stop, Abort, Mcp };
    struct OutboundMessage {
        OutboundKind kind;
        std::weak_ptr<Protocol> identity;
        uint64_t epoch;
        std::unique_ptr<AudioStreamPacket> audio;
        bool wake_word_data = false;
        std::string text;
        ListeningMode listening_mode = kListeningModeAutoStop;
        AbortReason abort_reason = kAbortReasonNone;
    };
    std::mutex outbound_mutex_;
    std::deque<OutboundMessage> outbound_messages_;
    size_t outbound_audio_count_ = 0;
    size_t outbound_audio_bytes_ = 0;
    size_t outbound_control_count_ = 0;
    size_t outbound_mcp_count_ = 0;
    size_t outbound_control_bytes_ = 0;
    bool outbound_pump_queued_ = false;
    std::atomic<uint64_t> outbound_failed_epoch_{0};
    NotifyPlayer notify_player_;
    uint32_t notification_playback_id_ = 0;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ =
        false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles);
    void StopNotification();
    void HandleNotificationFinished(uint32_t playback_id, bool success);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    std::shared_ptr<Protocol> ProtocolSnapshot() const;
    bool HasProtocol() const;
    bool IsCurrentProtocol(const std::weak_ptr<Protocol>& protocol, uint64_t epoch) const;
    bool IsCurrentProtocol(const std::shared_ptr<Protocol>& protocol, uint64_t epoch) const;
    bool IsProtocolChannelOpened();
    void RequestProtocolClose(bool reset = false, bool send_goodbye = true);
    void QueueProtocolOpen(std::function<void()> on_opened);
    void FinalizeProtocolOpen();
    void QueuePendingMcpMessage(uint64_t epoch, const std::string& payload);
    void FlushPendingMcpMessages(uint64_t epoch);
    void DispatchProtocolAction(std::weak_ptr<Protocol> identity, uint64_t epoch,
                                std::string session_id, std::function<void()> action);
    void FlushPendingProtocolActions();
    // Queue helpers are called by the main task; the pump alone touches the
    // transport. MCP saturation falls back to the main-task retry deque.
    bool QueueOutboundAudio(std::unique_ptr<AudioStreamPacket> packet, bool wake_word_data = false);
    bool QueueOutboundControl(OutboundKind kind, const std::string& text = "",
                              ListeningMode mode = kListeningModeAutoStop,
                              AbortReason reason = kAbortReasonNone);
    void ProcessOutboundMessages();
    void ClearOutboundMessages(bool audio_only = false, bool preserve_wake_word_data = false);
    template <typename Fn>
    bool TryWithProtocol(Fn&& fn) {
        std::unique_lock<std::mutex> io_lock(protocol_io_mutex_, std::try_to_lock);
        if (!io_lock.owns_lock() || protocol_work_pending_.load(std::memory_order_acquire) != 0)
            return false;
        auto protocol = ProtocolSnapshot();
        if (!protocol)
            return false;
        fn(*protocol);
        return true;
    }
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
