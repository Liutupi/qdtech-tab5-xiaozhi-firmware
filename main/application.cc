#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"
#include "wifi_manager.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <limits>

#define TAG "Application"

Application::Application() : notify_player_(audio_service_) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    notify_player_.Stop();
    if (outbound_task_) {
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            protocol_epoch_.fetch_add(1, std::memory_order_acq_rel);
            ClearOutboundMessages();
        }
        outbound_task_->WaitForCompletion();
        outbound_task_.reset();
    }
    if (background_task_) {
        background_task_->WaitForCompletion();
        background_task_.reset();
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

std::shared_ptr<Protocol> Application::ProtocolSnapshot() const {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    return protocol_;
}

bool Application::HasProtocol() const {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    return protocol_ != nullptr;
}

bool Application::IsCurrentProtocol(const std::weak_ptr<Protocol>& protocol, uint64_t epoch) const {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    if (!protocol_ || protocol_epoch_.load(std::memory_order_acquire) != epoch)
        return false;
    const std::weak_ptr<Protocol> current = protocol_;
    return !protocol.owner_before(current) && !current.owner_before(protocol);
}

bool Application::IsCurrentProtocol(const std::shared_ptr<Protocol>& protocol,
                                    uint64_t epoch) const {
    std::lock_guard<std::mutex> lock(protocol_mutex_);
    return protocol && protocol_ == protocol &&
           protocol_epoch_.load(std::memory_order_acquire) == epoch;
}

bool Application::IsProtocolChannelOpened() {
    bool opened = false;
    TryWithProtocol([&opened](Protocol& protocol) { opened = protocol.IsAudioChannelOpened(); });
    return opened;
}

void Application::RequestProtocolClose(bool reset, bool send_goodbye) {
    // CancelOpen only signals the handshake task. Actual socket teardown may
    // block and must run after any queued Open on the same worker. Publish the
    // invalidation and enqueue Close while holding the publication lock so a
    // later Open cannot overtake it.
    std::unique_lock<std::mutex> reset_lock(protocol_reset_mutex_, std::defer_lock);
    if (reset)
        reset_lock.lock();
    std::unique_lock<std::mutex> publication_lock(protocol_mutex_);
    auto protocol = protocol_;
    protocol_epoch_.fetch_add(1, std::memory_order_acq_rel);
    ClearOutboundMessages();
    if (!protocol)
        return;
    protocol_work_pending_.fetch_add(1, std::memory_order_acq_rel);
    protocol->CancelOpen();
    if (reset) {
        ++protocol_resets_pending_;
        protocol_.reset();
    }

    auto close = [this, protocol = std::move(protocol), reset, send_goodbye]() mutable {
        {
            std::lock_guard<std::mutex> lock(protocol_io_mutex_);
            suppress_close_callback_.store(true, std::memory_order_release);
            protocol->CloseAudioChannel(send_goodbye);
            suppress_close_callback_.store(false, std::memory_order_release);
            if (!IsExternalAudioActive() && GetDeviceState() != kDeviceStateConnecting)
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            // With reset, this worker should release the final application
            // reference before allowing activation to publish a replacement.
            protocol.reset();
        }
        protocol_work_pending_.fetch_sub(1, std::memory_order_acq_rel);
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
        if (reset) {
            {
                std::lock_guard<std::mutex> lock(protocol_reset_mutex_);
                --protocol_resets_pending_;
            }
            protocol_reset_cv_.notify_all();
        }
    };
    if (background_task_) {
        background_task_->Schedule(std::move(close));
    } else {
        publication_lock.unlock();
        if (reset)
            reset_lock.unlock();
        close();
    }
}

void Application::QueueProtocolOpen(std::function<void()> on_opened) {
    if (GetDeviceState() != kDeviceStateConnecting)
        return;
    std::unique_lock<std::mutex> publication_lock(protocol_mutex_);
    const auto protocol = protocol_;
    if (!protocol) {
        publication_lock.unlock();
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (protocol_open_queued_ &&
        protocol_open_epoch_ == protocol_epoch_.load(std::memory_order_acquire)) {
        protocol_open_completion_ = std::move(on_opened);
        ESP_LOGI(TAG, "replaced pending protocol open intent");
        return;
    }
    const uint64_t epoch = protocol_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    ClearOutboundMessages();
    protocol_open_queued_ = true;
    protocol_open_epoch_ = epoch;
    protocol_closed_during_open_epoch_ = 0;
    protocol_open_completion_ = std::move(on_opened);
    protocol_work_pending_.fetch_add(1, std::memory_order_acq_rel);
    auto open = [this, protocol, epoch]() {
        bool opened = false;
        {
            std::lock_guard<std::mutex> lock(protocol_io_mutex_);
            if (IsCurrentProtocol(protocol, epoch) && GetDeviceState() == kDeviceStateConnecting &&
                !IsExternalAudioActive()) {
                auto& board = Board::GetInstance();
                board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                board.PrepareForNetwork();
                if (auto* camera = board.GetCamera())
                    camera->PauseStream();
                opened = protocol->IsAudioChannelOpened() || protocol->OpenAudioChannel();
                if (opened) {
                    auto* codec = board.GetAudioCodec();
                    if (codec != nullptr &&
                        protocol->server_sample_rate() != codec->output_sample_rate()) {
                        ESP_LOGW(TAG,
                                 "Server sample rate %d does not match device output sample "
                                 "rate %d, resampling may cause distortion",
                                 protocol->server_sample_rate(), codec->output_sample_rate());
                    }
                }
            }
        }
        protocol_work_pending_.fetch_sub(1, std::memory_order_acq_rel);
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
        const std::weak_ptr<Protocol> identity = protocol;
        Schedule([this, identity, epoch, opened]() {
            pending_open_completion_ = PendingOpenCompletion{identity, epoch, opened};
            FinalizeProtocolOpen();
        });
    };
    if (background_task_) {
        background_task_->Schedule(std::move(open));
    } else {
        publication_lock.unlock();
        open();
    }
}

void Application::FinalizeProtocolOpen() {
    if (!pending_open_completion_)
        return;
    const auto completion = *pending_open_completion_;
    const auto finish = [this, epoch = completion.epoch]() {
        std::function<void()> on_opened;
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            if (protocol_open_epoch_ == epoch) {
                protocol_open_queued_ = false;
                on_opened = std::move(protocol_open_completion_);
            }
        }
        pending_open_completion_.reset();
        return on_opened;
    };
    if (!IsCurrentProtocol(completion.identity, completion.epoch)) {
        finish();
        return;
    }
    if (GetDeviceState() != kDeviceStateConnecting || IsExternalAudioActive()) {
        finish();
        if (completion.opened)
            RequestProtocolClose(false, false);
        return;
    }
    if (!completion.opened) {
        finish();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    bool channel_open = false;
    if (!TryWithProtocol(
            [&channel_open](Protocol& current) { channel_open = current.IsAudioChannelOpened(); }))
        return;  // Worker is busy; MAIN_EVENT_PLAYBACK_DRAINED retries.
    auto on_opened = finish();
    if (!IsCurrentProtocol(completion.identity, completion.epoch))
        return;
    {
        std::lock_guard<std::mutex> lock(protocol_mutex_);
        if (protocol_closed_during_open_epoch_ == completion.epoch)
            channel_open = false;
    }
    if (!channel_open) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (on_opened)
        on_opened();
    FlushPendingProtocolActions();
    FlushPendingMcpMessages(completion.epoch);
}

void Application::QueuePendingMcpMessage(uint64_t epoch, const std::string& payload) {
    constexpr size_t kMaxQueuedMessages = 4;
    constexpr size_t kMaxQueuedPayloadBytes = 12 * 1024;
    constexpr size_t kMaxQueuedBytes = 32 * 1024;
    if (payload.size() > kMaxQueuedPayloadBytes) {
        ESP_LOGW(TAG, "dropping oversized deferred MCP response: %zu bytes", payload.size());
        return;
    }
    size_t queued_bytes = 0;
    for (const auto& item : pending_mcp_messages_)
        queued_bytes += item.second.size();
    while (!pending_mcp_messages_.empty() && (pending_mcp_messages_.size() >= kMaxQueuedMessages ||
                                              queued_bytes + payload.size() > kMaxQueuedBytes)) {
        ESP_LOGW(TAG, "dropping oldest deferred MCP response (queue limit)");
        queued_bytes -= pending_mcp_messages_.front().second.size();
        pending_mcp_messages_.pop_front();
    }
    pending_mcp_messages_.emplace_back(epoch, payload);
}

void Application::FlushPendingMcpMessages(uint64_t epoch) {
    if (epoch != protocol_epoch_.load(std::memory_order_acquire))
        return;
    if (pending_mcp_messages_.empty())
        return;
    auto pending = std::move(pending_mcp_messages_);
    pending_mcp_messages_.clear();
    for (auto& [message_epoch, payload] : pending) {
        if (message_epoch != epoch)
            continue;
        if (!QueueOutboundControl(OutboundKind::Mcp, payload))
            QueuePendingMcpMessage(message_epoch, payload);
    }
}

void Application::DispatchProtocolAction(std::weak_ptr<Protocol> identity, uint64_t epoch,
                                         std::string session_id, std::function<void()> action) {
    if (!IsCurrentProtocol(identity, epoch))
        return;
    if (!session_id.empty()) {
        bool opening = false;
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            opening = protocol_open_queued_ && protocol_open_epoch_ == epoch;
        }
        bool matches = false;
        if (opening || !TryWithProtocol([&](Protocol& current) {
                matches = current.session_id() == session_id && current.IsAudioChannelOpened();
            })) {
            constexpr size_t kMaxPendingProtocolActions = 8;
            if (pending_protocol_actions_.size() == kMaxPendingProtocolActions) {
                ESP_LOGW(TAG, "dropping oldest deferred protocol action (queue full)");
                pending_protocol_actions_.pop_front();
            }
            pending_protocol_actions_.push_back(
                {std::move(identity), epoch, std::move(session_id), std::move(action)});
            return;
        }
        if (!matches)
            return;
    }
    action();
}

void Application::FlushPendingProtocolActions() {
    if (pending_protocol_actions_.empty())
        return;
    auto pending = std::move(pending_protocol_actions_);
    pending_protocol_actions_.clear();
    for (auto& item : pending) {
        DispatchProtocolAction(std::move(item.identity), item.epoch, std::move(item.session_id),
                               std::move(item.action));
    }
}

bool Application::QueueOutboundAudio(std::unique_ptr<AudioStreamPacket> packet,
                                     bool wake_word_data) {
    if (!packet)
        return false;
    if (!outbound_task_) {
        bool sent = false;
        return TryWithProtocol(
                   [&](Protocol& protocol) { sent = protocol.SendAudio(std::move(packet)); }) &&
               sent;
    }

    // A short live tail keeps speech responsive after a slow send. Wake-word
    // preroll may span longer, so it can use the audio service's full bound.
    const size_t max_audio_packets = wake_word_data
                                         ? MAX_SEND_PACKETS_IN_QUEUE
                                         : std::min<size_t>(16, MAX_SEND_PACKETS_IN_QUEUE);
    constexpr size_t kMaxAudioBytes = 16 * 1024;
    const size_t bytes = packet->payload.size();
    if (bytes > kMaxAudioBytes)
        return false;

    std::unique_lock<std::mutex> publication_lock(protocol_mutex_);
    if (!protocol_ || protocol_work_pending_.load(std::memory_order_acquire) != 0 ||
        outbound_failed_epoch_.load(std::memory_order_acquire) ==
            protocol_epoch_.load(std::memory_order_acquire))
        return false;
    OutboundMessage message{};
    message.kind = OutboundKind::Audio;
    message.identity = protocol_;
    message.epoch = protocol_epoch_.load(std::memory_order_acquire);
    message.audio = std::move(packet);
    message.wake_word_data = wake_word_data;

    bool schedule_pump = false;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        while (outbound_audio_count_ >= max_audio_packets ||
               outbound_audio_bytes_ + bytes > kMaxAudioBytes) {
            auto oldest =
                std::find_if(outbound_messages_.begin(), outbound_messages_.end(),
                             [](const OutboundMessage& item) {
                                 return item.kind == OutboundKind::Audio && !item.wake_word_data;
                             });
            if (oldest == outbound_messages_.end())
                return false;
            outbound_audio_bytes_ -= oldest->audio->payload.size();
            --outbound_audio_count_;
            outbound_messages_.erase(oldest);
            ESP_LOGW(TAG, "dropping stale outbound audio packet (queue full)");
        }
        outbound_audio_bytes_ += bytes;
        ++outbound_audio_count_;
        outbound_messages_.push_back(std::move(message));
        if (!outbound_pump_queued_) {
            outbound_pump_queued_ = true;
            schedule_pump = true;
        }
    }
    publication_lock.unlock();
    if (schedule_pump)
        outbound_task_->Schedule([this]() { ProcessOutboundMessages(); });
    return true;
}

bool Application::QueueOutboundControl(OutboundKind kind, const std::string& text,
                                       ListeningMode mode, AbortReason reason) {
    if (!outbound_task_) {
        bool sent = false;
        return TryWithProtocol([&](Protocol& protocol) {
            switch (kind) {
                case OutboundKind::WakeWord:
                    sent = protocol.SendWakeWordDetected(text);
                    break;
                case OutboundKind::Start:
                    sent = protocol.SendStartListening(mode);
                    break;
                case OutboundKind::Stop:
                    sent = protocol.SendStopListening();
                    break;
                case OutboundKind::Abort:
                    sent = protocol.SendAbortSpeaking(reason);
                    break;
                case OutboundKind::Mcp:
                    sent = protocol.SendMcpMessage(text);
                    break;
                case OutboundKind::Audio:
                    break;
            }
        }) && sent;
    }

    constexpr size_t kMaxControlMessages = 8;
    constexpr size_t kMaxControlBytes = 16 * 1024;
    constexpr size_t kMaxControlPayloadBytes = 12 * 1024;
    if (kind == OutboundKind::Audio || text.size() > kMaxControlPayloadBytes)
        return false;

    std::unique_lock<std::mutex> publication_lock(protocol_mutex_);
    if (!protocol_ || protocol_work_pending_.load(std::memory_order_acquire) != 0 ||
        outbound_failed_epoch_.load(std::memory_order_acquire) ==
            protocol_epoch_.load(std::memory_order_acquire))
        return false;
    OutboundMessage message{};
    message.kind = kind;
    message.identity = protocol_;
    message.epoch = protocol_epoch_.load(std::memory_order_acquire);
    message.text = text;
    message.listening_mode = mode;
    message.abort_reason = reason;

    bool schedule_pump = false;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        // Reserve half the control slots for time-sensitive listen/abort
        // commands. MCP replies use the separate bounded retry queue when full.
        if ((kind == OutboundKind::Mcp && outbound_mcp_count_ >= 4) ||
            outbound_control_count_ >= kMaxControlMessages ||
            outbound_control_bytes_ + text.size() > kMaxControlBytes)
            return false;
        ++outbound_control_count_;
        if (kind == OutboundKind::Mcp)
            ++outbound_mcp_count_;
        outbound_control_bytes_ += text.size();
        if (kind == OutboundKind::Stop || kind == OutboundKind::Abort) {
            // The caller drops queued audio first; stop/abort must not wait
            // behind unrelated MCP replies.
            auto first_mcp = std::find_if(
                outbound_messages_.begin(), outbound_messages_.end(),
                [](const OutboundMessage& item) { return item.kind == OutboundKind::Mcp; });
            outbound_messages_.insert(first_mcp, std::move(message));
        } else {
            outbound_messages_.push_back(std::move(message));
        }
        if (!outbound_pump_queued_) {
            outbound_pump_queued_ = true;
            schedule_pump = true;
        }
    }
    publication_lock.unlock();
    if (schedule_pump)
        outbound_task_->Schedule([this]() { ProcessOutboundMessages(); });
    return true;
}

void Application::ProcessOutboundMessages() {
    while (true) {
        OutboundMessage message{};
        bool control_slot_freed = false;
        {
            std::lock_guard<std::mutex> lock(outbound_mutex_);
            if (outbound_messages_.empty()) {
                outbound_pump_queued_ = false;
                return;
            }
            message = std::move(outbound_messages_.front());
            outbound_messages_.pop_front();
            if (message.kind == OutboundKind::Audio) {
                --outbound_audio_count_;
                outbound_audio_bytes_ -= message.audio->payload.size();
            } else {
                --outbound_control_count_;
                if (message.kind == OutboundKind::Mcp)
                    --outbound_mcp_count_;
                outbound_control_bytes_ -= message.text.size();
                control_slot_freed = true;
            }
        }
        // A waiting listen start or MCP response can retry as soon as a
        // control slot has been freed. Wake the main task outside the queue lock.
        if (control_slot_freed)
            xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);

        bool failed_send = false;
        {
            std::lock_guard<std::mutex> lock(protocol_io_mutex_);
            if (!IsCurrentProtocol(message.identity, message.epoch))
                continue;
            auto protocol = message.identity.lock();
            if (!protocol)
                continue;
            switch (message.kind) {
                case OutboundKind::Audio:
                    failed_send = !protocol->SendAudio(std::move(message.audio));
                    break;
                case OutboundKind::WakeWord:
                    failed_send = !protocol->SendWakeWordDetected(message.text);
                    break;
                case OutboundKind::Start:
                    failed_send = !protocol->SendStartListening(message.listening_mode);
                    break;
                case OutboundKind::Stop:
                    failed_send = !protocol->SendStopListening();
                    break;
                case OutboundKind::Abort:
                    failed_send = !protocol->SendAbortSpeaking(message.abort_reason);
                    break;
                case OutboundKind::Mcp:
                    failed_send = !protocol->SendMcpMessage(message.text);
                    break;
            }
        }
        if (failed_send) {
            bool current_failure = false;
            {
                // A slow old send can fail after a replacement session has
                // already queued its Start/MCP messages. Never clear that
                // newer queue or poison its epoch with the old failure.
                std::lock_guard<std::mutex> publication_lock(protocol_mutex_);
                if (protocol_ && protocol_ == message.identity.lock() &&
                    protocol_epoch_.load(std::memory_order_acquire) == message.epoch) {
                    outbound_failed_epoch_.store(message.epoch, std::memory_order_release);
                    ClearOutboundMessages();
                    current_failure = true;
                }
            }
            if (!current_failure)
                continue;
            const auto identity = message.identity;
            const auto epoch = message.epoch;
            Schedule([this, identity, epoch]() {
                if (IsCurrentProtocol(identity, epoch)) {
                    RequestProtocolClose(false, false);
                    auto state = GetDeviceState();
                    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                        state == kDeviceStateSpeaking)
                        SetDeviceState(kDeviceStateIdle);
                }
            });
        }
    }
}

void Application::ClearOutboundMessages(bool audio_only, bool preserve_wake_word_data) {
    if (!outbound_task_)
        return;
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    if (!audio_only) {
        outbound_messages_.clear();
        outbound_audio_count_ = 0;
        outbound_audio_bytes_ = 0;
        outbound_control_count_ = 0;
        outbound_mcp_count_ = 0;
        outbound_control_bytes_ = 0;
        return;
    }
    for (auto it = outbound_messages_.begin(); it != outbound_messages_.end();) {
        if (it->kind != OutboundKind::Audio || (preserve_wake_word_data && it->wake_word_data)) {
            ++it;
            continue;
        }
        --outbound_audio_count_;
        outbound_audio_bytes_ -= it->audio->payload.size();
        it = outbound_messages_.erase(it);
    }
}

void Application::CloseAudioChannelAsync(bool send_goodbye) {
    Schedule([this, send_goodbye]() { RequestProtocolClose(false, send_goodbye); });
}

void Application::CloseAudioChannelAsync(bool send_goodbye,
                                         std::function<bool(Protocol&)> still_current) {
    std::unique_lock<std::mutex> publication_lock(protocol_mutex_);
    auto protocol = protocol_;
    if (!protocol)
        return;
    const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
    protocol_work_pending_.fetch_add(1, std::memory_order_acq_rel);
    auto close = [this, protocol, epoch, send_goodbye,
                  still_current = std::move(still_current)]() mutable {
        {
            std::lock_guard<std::mutex> lock(protocol_io_mutex_);
            if (IsCurrentProtocol(protocol, epoch) && still_current(*protocol)) {
                bool close_current = false;
                uint64_t closed_epoch = 0;
                {
                    std::lock_guard<std::mutex> publication_lock(protocol_mutex_);
                    if (protocol_ == protocol &&
                        protocol_epoch_.load(std::memory_order_acquire) == epoch) {
                        closed_epoch = protocol_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
                        close_current = true;
                    }
                }
                if (close_current) {
                    ClearOutboundMessages();
                    protocol->CancelOpen();
                    suppress_close_callback_.store(true, std::memory_order_release);
                    protocol->CloseAudioChannel(send_goodbye);
                    suppress_close_callback_.store(false, std::memory_order_release);
                    if (!IsExternalAudioActive() && GetDeviceState() != kDeviceStateConnecting)
                        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                    const std::weak_ptr<Protocol> identity = protocol;
                    Schedule([this, identity, closed_epoch]() {
                        if (!IsCurrentProtocol(identity, closed_epoch))
                            return;
                        auto state = GetDeviceState();
                        if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                            state == kDeviceStateSpeaking)
                            SetDeviceState(kDeviceStateIdle);
                    });
                }
            }
        }
        protocol_work_pending_.fetch_sub(1, std::memory_order_acq_rel);
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    if (background_task_) {
        background_task_->Schedule(std::move(close));
    } else {
        publication_lock.unlock();
        Schedule(std::move(close));
    }
}

void Application::ScheduleProtocolMaintenance(std::function<void(Protocol&)> operation) {
    std::unique_lock<std::mutex> publication_lock(protocol_mutex_);
    auto protocol = protocol_;
    if (!protocol)
        return;
    const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
    if (protocol_maintenance_queued_ && protocol_maintenance_epoch_ == epoch)
        return;
    protocol_maintenance_queued_ = true;
    protocol_maintenance_epoch_ = epoch;
    protocol_work_pending_.fetch_add(1, std::memory_order_acq_rel);
    auto work = [this, protocol, epoch, operation = std::move(operation)]() mutable {
        {
            std::lock_guard<std::mutex> lock(protocol_io_mutex_);
            if (IsCurrentProtocol(protocol, epoch) && GetDeviceState() == kDeviceStateIdle)
                operation(*protocol);
        }
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            if (protocol_maintenance_epoch_ == epoch)
                protocol_maintenance_queued_ = false;
        }
        protocol_work_pending_.fetch_sub(1, std::memory_order_acq_rel);
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
        Schedule([this, epoch]() { FlushPendingMcpMessages(epoch); });
    };
    if (background_task_) {
        background_task_->Schedule(std::move(work));
    } else {
        publication_lock.unlock();
        Schedule(std::move(work));
    }
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

#if CONFIG_BOARD_TYPE_QDTECH_TAB5
    // QDTech image and SD work runs off the LVGL and audio tasks.
    background_task_ = std::make_unique<BackgroundTask>(12 * 1024);
    // Keep network writes independent of image/SD work and of channel teardown.
    // One bounded pump job is queued at a time; audio is never sent from main.
    outbound_task_ = std::make_unique<BackgroundTask>(6 * 1024);
#endif

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    ESP_LOGI(TAG, "After board/audio init");
    SystemInfo::PrintHeapStats();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            FinalizeProtocolOpen();
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
            FlushPendingProtocolActions();
            if (!pending_mcp_messages_.empty())
                FlushPendingMcpMessages(protocol_epoch_.load(std::memory_order_acquire));
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!QueueOutboundAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            FinalizeProtocolOpen();
            FlushPendingProtocolActions();
            if (!pending_mcp_messages_.empty())
                FlushPendingMcpMessages(protocol_epoch_.load(std::memory_order_acquire));
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        RequestProtocolClose(false, false);
        SetDeviceState(kDeviceStateIdle);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    // Protocol start may have already raised MAIN_EVENT_ERROR. Do not replace
    // that alert with the "ready" UI/sound — the main loop can process both
    // events back-to-back because the activation task is lower priority.
    const bool has_error = !last_error_message_.empty() ||
                           pending_protocol_error_epoch_.load(std::memory_order_acquire) ==
                               protocol_epoch_.load(std::memory_order_acquire);
    if (!has_error) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (!has_error) {
        Schedule([this]() {
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        });
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Initialize the protocol FIRST so voice/chat works even when the
    // version check cannot reach the server (TLS / low-memory failures).
    InitializeProtocol();

    // Check for new assets / firmware in the background path after the
    // protocol is ready. Failures must not block conversation.
    CheckAssetsVersion();
    CheckNewVersion();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 3;
    int retry_count = 0;
    int retry_delay = 5;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        auto check = ota_->CheckVersion();
        if (!check) {
            retry_count++;
            // Stop early if the user is already talking — never block chat.
            if (GetDeviceState() != kDeviceStateIdle) {
                ESP_LOGW(TAG, "Skip version check retries while device is busy");
                return;
            }
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                display->SetStatus(Lang::Strings::STANDBY);
                return;
            }

            const auto& err = check.error();
            char error_message[160];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "%s", err.ToString().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "%s", err.Message());
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "%s", err.Message());
            }
            // Only alert on the first failure so the UI is not stuck on this message.
            if (retry_count == 1) {
                Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);
            }

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() != kDeviceStateIdle) {
                    return;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 5;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            display->SetStatus(Lang::Strings::STANDBY);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Activation runs on its own task. A Wi-Fi reset may still be closing the
    // previous protocol on the network worker; wait here, never in Run().
    if (HasProtocol())
        RequestProtocolClose(true, false);
    {
        std::unique_lock<std::mutex> lock(protocol_reset_mutex_);
        protocol_reset_cv_.wait(lock, [this]() { return protocol_resets_pending_ == 0; });
    }
    const uint64_t initialization_epoch = protocol_epoch_.load(std::memory_order_acquire);
    std::shared_ptr<Protocol> protocol;
    if (ota_->HasMqttConfig()) {
        protocol = std::make_shared<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol = std::make_shared<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol = std::make_shared<MqttProtocol>();
    }
    // Callbacks carry an identity, not a weak_ptr lock. Reset may release the
    // application reference while a transport callback is still on its own
    // stack; making that callback the last shared owner would run the protocol
    // destructor on the callback stack and deadlock its in-flight gate.
    const std::weak_ptr<Protocol> weak_protocol_identity = protocol;

    protocol->OnConnected([this, weak_protocol_identity]() {
        const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
        if (!IsCurrentProtocol(weak_protocol_identity, epoch))
            return;
        Schedule([this, weak_protocol_identity, epoch]() {
            if (IsCurrentProtocol(weak_protocol_identity, epoch))
                DismissAlert();
        });
    });

    protocol->OnNetworkError([this, weak_protocol_identity](const std::string& message) {
        const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
        if (!IsCurrentProtocol(weak_protocol_identity, epoch))
            return;
        pending_protocol_error_epoch_.store(epoch, std::memory_order_release);
        Schedule([this, weak_protocol_identity, epoch, message]() {
            if (!IsCurrentProtocol(weak_protocol_identity, epoch)) {
                uint64_t expected = epoch;
                pending_protocol_error_epoch_.compare_exchange_strong(expected, 0);
                return;
            }
            last_error_message_ = message;
            uint64_t expected = epoch;
            pending_protocol_error_epoch_.compare_exchange_strong(expected, 0);
            xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        });
    });

    protocol->OnIncomingAudio(
        [this, weak_protocol_identity](std::unique_ptr<AudioStreamPacket> packet) {
            const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
            if (IsCurrentProtocol(weak_protocol_identity, epoch) &&
                GetDeviceState() == kDeviceStateSpeaking && !IsExternalAudioActive()) {
                audio_service_.PushPacketToDecodeQueue(std::move(packet));
            }
        });

    protocol->OnAudioChannelClosed([this, weak_protocol_identity, &board]() {
        if (suppress_close_callback_.load(std::memory_order_acquire))
            return;
        const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
        if (!IsCurrentProtocol(weak_protocol_identity, epoch))
            return;
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            if (protocol_open_queued_ && protocol_open_epoch_ == epoch) {
                protocol_closed_during_open_epoch_ = epoch;
                return;
            }
        }
        Schedule([this, weak_protocol_identity, epoch, &board]() {
            if (!IsCurrentProtocol(weak_protocol_identity, epoch))
                return;
            {
                std::lock_guard<std::mutex> lock(protocol_mutex_);
                if (protocol_open_queued_ && protocol_open_epoch_ == epoch) {
                    protocol_closed_during_open_epoch_ = epoch;
                    return;
                }
            }
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            ClearOutboundMessages();
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol->OnIncomingJson([this, display, weak_protocol_identity](const cJSON* root) {
        const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
        if (!IsCurrentProtocol(weak_protocol_identity, epoch))
            return;
        auto session_id = cJSON_GetObjectItem(root, "session_id");
        const std::string message_session =
            cJSON_IsString(session_id) ? session_id->valuestring : "";
        auto schedule_current = [this, weak_protocol_identity, epoch,
                                 message_session](std::function<void()> action) {
            Schedule([this, weak_protocol_identity, epoch, message_session,
                      action = std::move(action)]() mutable {
                DispatchProtocolAction(weak_protocol_identity, epoch, message_session,
                                       std::move(action));
            });
        };
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            schedule_current([this, url = std::string(audio_url->valuestring),
                              subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            if (IsExternalAudioActive()) {
                return;
            }
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                schedule_current([this]() {
                    if (IsExternalAudioActive()) {
                        return;
                    }
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                schedule_current([this]() {
                    if (IsExternalAudioActive()) {
                        return;
                    }
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    schedule_current([display, message = std::string(text->valuestring),
                                      glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                schedule_current([display, message = std::string(text->valuestring),
                                  glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                schedule_current([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            // MCP tools may have device side effects. Require the server's
            // established session, including during a WebSocket hello race.
            if (message_session.empty()) {
                ESP_LOGW(TAG, "Ignoring MCP message without session_id");
                return;
            }
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    schedule_current([this, weak_protocol_identity, epoch, message_session,
                                      payload_str = std::string(payload_json.get())]() {
                        McpServer::GetInstance().ParseMessage(
                            payload_str, [this, weak_protocol_identity, epoch,
                                          message_session](const std::string& response) {
                                Schedule([this, weak_protocol_identity, epoch, message_session,
                                          response]() {
                                    DispatchProtocolAction(
                                        weak_protocol_identity, epoch, message_session,
                                        [this, epoch, response]() {
                                            if (!QueueOutboundControl(OutboundKind::Mcp, response))
                                                QueuePendingMcpMessage(epoch, response);
                                        });
                                });
                            });
                    });
                }
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    schedule_current([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                schedule_current([this, status = std::string(status->valuestring),
                                  message = std::string(message->valuestring),
                                  emotion = std::string(emotion->valuestring)]() {
                    Alert(status.c_str(), message.c_str(), emotion.c_str(),
                          Lang::Sounds::OGG_VIBRATION);
                });
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    schedule_current(
                        [this, display, payload_str = std::string(payload_json.get())]() {
                            display->SetChatMessage("system", payload_str.c_str());
                        });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    // A reset could arrive between the wait above and publication. Recheck
    // under the short reset lock after obtaining the I/O lock; never hold the
    // reset lock across Start(), which can perform a slow MQTT connect.
    for (;;) {
        std::unique_lock<std::mutex> io_lock(protocol_io_mutex_);
        std::unique_lock<std::mutex> reset_lock(protocol_reset_mutex_);
        if (protocol_resets_pending_ != 0) {
            reset_lock.unlock();
            io_lock.unlock();
            std::unique_lock<std::mutex> wait_lock(protocol_reset_mutex_);
            protocol_reset_cv_.wait(wait_lock, [this]() { return protocol_resets_pending_ == 0; });
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(protocol_mutex_);
            if (protocol_epoch_.load(std::memory_order_acquire) != initialization_epoch) {
                ESP_LOGW(TAG, "protocol initialization superseded by reset");
                return;
            }
            protocol_ = protocol;
            protocol_epoch_.fetch_add(1, std::memory_order_acq_rel);
        }
        reset_lock.unlock();
        protocol->Start();
        break;
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    last_error_message_.clear();
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!HasProtocol()) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!IsProtocolChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        RequestProtocolClose();
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    QueueProtocolOpen([this, mode]() { SetListeningMode(mode); });
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!HasProtocol()) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!IsProtocolChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        if (AbortSpeaking(kAbortReasonNone))
            SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        ClearOutboundMessages(true);
        if (!QueueOutboundControl(OutboundKind::Stop))
            RequestProtocolClose(false, false);
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (IsExternalAudioActive()) {
        return;
    }
    if (!HasProtocol()) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
        StopNotification();
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        if (!AbortSpeaking(kAbortReasonWakeWordDetected)) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;
        ClearOutboundMessages(true);

        if (state == kDeviceStateListening) {
            if (!QueueOutboundControl(OutboundKind::Start, "", GetDefaultListeningMode())) {
                RequestProtocolClose(false, false);
                SetDeviceState(kDeviceStateIdle);
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!IsProtocolChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    auto after_open = [this, wake_word]() {
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        bool queued = true;
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            if (!QueueOutboundAudio(std::move(packet), true)) {
                queued = false;
                break;
            }
        }
        if (!queued || !QueueOutboundControl(OutboundKind::WakeWord, wake_word)) {
            ClearOutboundMessages(true);
            RequestProtocolClose(false, false);
            SetDeviceState(kDeviceStateIdle);
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
        SetListeningMode(GetDefaultListeningMode());
#else
        play_popup_on_listening_ = true;
        SetListeningMode(GetDefaultListeningMode());
#endif
    };

    if (IsProtocolChannelOpened()) {
        after_open();
    } else {
        QueueProtocolOpen(std::move(after_open));
    }
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Keep a just-raised network error visible. SetDeviceState(idle)
            // queues STATE_CHANGED after Alert(), and the idle handler would
            // otherwise wipe the status, emotion, and chat message.
            if (last_error_message_.empty()) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->SetEmotion(
                    "neutral");  // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(!external_audio_active_.load());
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    if (IsExternalAudioActive()) {
        pending_listening_start_ = true;
        return;
    }

    // Send the start listening command
    ClearOutboundMessages(true, true);
    if (!QueueOutboundControl(OutboundKind::Start, "", listening_mode_)) {
        if (outbound_task_) {
            pending_listening_start_ = true;
        } else {
            // On boards without the outbound worker a false result means the
            // transport rejected Start; there is no queue slot to wait for.
            RequestProtocolClose(false, false);
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            Schedule([this, id, text]() {
                if (GetDeviceState() == kDeviceStateNotifying && notification_playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        });

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
    }
}

void Application::StopNotification() {
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    StopNotification();
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

bool Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    ClearOutboundMessages(true);
    if (QueueOutboundControl(OutboundKind::Abort, "", kListeningModeAutoStop, reason))
        return true;
    RequestProtocolClose(false, false);
    SetDeviceState(kDeviceStateIdle);
    return false;
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Do not destroy a protocol while its background handshake is still
    // running. The worker owns the snapshot until Close completes or reboot.
    RequestProtocolClose(true);
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version,
                                  const std::string& expected_sha256) {
#ifdef CONFIG_BOARD_TYPE_QDTECH_TAB5
    // The Tab5 has a single 12 MB firmware slot; only the SD-staged updater (Settings ->
    // 固件升级) may replace it. Cloud-pushed or MCP-requested upgrades are refused.
    ESP_LOGW(TAG, "Cloud firmware upgrade is disabled on Tab5 (url=%s)", url.c_str());
    (void)version;
    (void)expected_sha256;
    return false;
#endif
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (HasProtocol()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        RequestProtocolClose();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(
        upgrade_url,
        [this, display](int progress, size_t speed) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        },
        expected_sha256);

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

bool Application::InvokeTextCommand(const std::string& text) {
    if (!HasProtocol() || text.empty()) {
        return false;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    Schedule([this, text]() {
        if (GetDeviceState() != kDeviceStateIdle || !HasProtocol()) {
            ESP_LOGW(TAG, "text command dropped (state %d): %s", (int)GetDeviceState(),
                     text.c_str());
            return;
        }
        if (!SetDeviceState(kDeviceStateConnecting)) {
            return;
        }
        QueueProtocolOpen([this, text]() {
            ESP_LOGI(TAG, "text command: %s", text.c_str());
            if (!QueueOutboundControl(OutboundKind::WakeWord, text)) {
                RequestProtocolClose(false, false);
                SetDeviceState(kDeviceStateIdle);
                return;
            }
            SetListeningMode(GetDefaultListeningMode());
        });
    });
    return true;
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!HasProtocol()) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            RequestProtocolClose();
            SetDeviceState(kDeviceStateIdle);
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_work_pending_.load(std::memory_order_acquire) != 0) {
        return false;
    }
    if (HasProtocol()) {
        bool opened = false;
        if (!TryWithProtocol(
                [&opened](Protocol& protocol) { opened = protocol.IsAudioChannelOpened(); }) ||
            opened)
            return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    const uint64_t epoch = protocol_epoch_.load(std::memory_order_acquire);
    Schedule([this, payload, epoch]() {
        if (epoch == protocol_epoch_.load(std::memory_order_acquire) && HasProtocol()) {
            if (!QueueOutboundControl(OutboundKind::Mcp, payload))
                QueuePendingMcpMessage(epoch, payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (HasProtocol())
            RequestProtocolClose();
        auto state = GetDeviceState();
        if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
            state == kDeviceStateSpeaking)
            SetDeviceState(kDeviceStateIdle);
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::RegisterDeviceStateCallback(
    std::function<void(DeviceState, DeviceState)> callback) {
    if (callback) state_machine_.AddStateChangeListener(std::move(callback));
}

void Application::SetExternalAudioActive(bool active) {
    // Peak current of speaker PA + LCD + Wi-Fi TX has triggered BOD reboots on
    // USB power. Dim the backlight only — never touch the user's volume.
    static int saved_brightness = -1;
    constexpr int kExtAudioMaxBrightness = 40;

    const bool previous = external_audio_active_.exchange(active);
    audio_service_.SetExternalPlaybackActive(active);
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        codec->SetExternalPlaybackActive(active);
    }
    if (active && !previous) {
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.ResetDecoder();
        // BALANCED cuts Wi-Fi latency without the extra current of PERFORMANCE.
        // PERFORMANCE + speaker PA has triggered BOD (brownout) reboots on USB.
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::BALANCED);
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            saved_brightness = backlight->brightness();
            if (saved_brightness > kExtAudioMaxBrightness) {
                ESP_LOGI(TAG, "external audio: dim backlight %d -> %d", saved_brightness,
                         kExtAudioMaxBrightness);
                backlight->SetBrightness(kExtAudioMaxBrightness, false);
            } else {
                saved_brightness = -1;
            }
        }
    } else if (!active && previous) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (saved_brightness >= 0) {
            if (auto* backlight = Board::GetInstance().GetBacklight()) {
                if (backlight->brightness() == kExtAudioMaxBrightness) {
                    backlight->SetBrightness(saved_brightness, false);
                }
            }
            saved_brightness = -1;
        }
        // Defer mic bring-up. A stream reconnect can briefly clear the flag;
        // re-enabling the mic reconfigures shared duplex I2S and can kill TX.
        Schedule([this]() {
            if (external_audio_active_.load()) {
                return;
            }
            if (GetDeviceState() == kDeviceStateListening) {
                pending_listening_start_ = false;
                StartListeningAudio();
                return;
            }
            if (GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            audio_service_.EnableWakeWordDetection(true);
        });
    }
}

void Application::PrepareExternalAudioPlayback() {
    SetExternalAudioActive(true);
    Schedule([this]() {
        if (!IsExternalAudioActive()) {
            return;
        }
        pending_listening_start_ = false;
        play_popup_on_listening_ = false;
        if (HasProtocol())
            RequestProtocolClose(false, false);
        if (GetDeviceState() == kDeviceStateSpeaking || GetDeviceState() == kDeviceStateListening ||
            GetDeviceState() == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        RequestProtocolClose(true);
        if (GetDeviceState() == kDeviceStateConnecting ||
            GetDeviceState() == kDeviceStateListening || GetDeviceState() == kDeviceStateSpeaking)
            SetDeviceState(kDeviceStateIdle);
    });
}
