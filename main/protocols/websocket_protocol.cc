#include "websocket_protocol.h"
#include "audio_service.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <limits>
#include <utility>
#include "assets/lang_config.h"

#define TAG "WS"

namespace {
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F callback) : callback_(std::move(callback)) {}
    ~ScopeExit() { callback_(); }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F callback_;
};
}  // namespace

WebsocketProtocol::WebsocketProtocol() = default;

WebsocketProtocol::~WebsocketProtocol() { CloseAudioChannel(); }

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr) {
        return false;
    }

    std::shared_ptr<WebSocket> websocket;
    std::shared_ptr<ConnectionAttempt> attempt;
    int version;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!opened_ || error_occurred_ || websocket_ == nullptr || attempt_ == nullptr ||
            attempt_->disconnected) {
            return false;
        }
        websocket = websocket_;
        attempt = attempt_;
        version = attempt->version;
        ++active_sends_;
    }
    auto release_send = [this, &websocket]() {
        websocket.reset();
        FinishSend();
    };
    ScopeExit<decltype(release_send)> send_guard(release_send);

    std::lock_guard<std::mutex> send_lock(send_mutex_);
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected || !opened_) {
            return false;
        }
    }

    if (version == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket->Send(serialized.data(), serialized.size(), true);
    } else if (version == 3) {
        if (packet->payload.size() > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    std::shared_ptr<WebSocket> websocket;
    std::shared_ptr<ConnectionAttempt> attempt;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!opened_ || error_occurred_ || websocket_ == nullptr || attempt_ == nullptr ||
            attempt_->disconnected) {
            return false;
        }
        websocket = websocket_;
        attempt = attempt_;
        ++active_sends_;
    }

    bool sent;
    {
        auto release_send = [this, &websocket]() {
            websocket.reset();
            FinishSend();
        };
        ScopeExit<decltype(release_send)> send_guard(release_send);
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (!IsCurrentLocked(attempt) || attempt->disconnected || !opened_) {
                return false;
            }
        }
        sent = websocket->Send(text);
    }
    if (!sent) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        ReportError(attempt, Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return opened_ && websocket_ != nullptr && attempt_ != nullptr && !attempt_->disconnected &&
           !error_occurred_ && !IsTimeout();
}

bool WebsocketProtocol::IsCurrentLocked(const std::shared_ptr<ConnectionAttempt>& attempt) const {
    return attempt != nullptr && attempt_ == attempt && generation_ == attempt->generation;
}

void WebsocketProtocol::FinishSend() {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    --active_sends_;
    send_done_.notify_all();
}

void WebsocketProtocol::CancelOpen() {
    std::shared_ptr<ConnectionAttempt> canceled;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        ++generation_;
        // A queued Open may not have entered yet. Its entry consumes this once.
        if (!opening_) {
            cancel_next_open_ = true;
        }
        canceled = std::move(attempt_);
        opened_ = false;
        // Keep websocket_ owned by this protocol while signaling. Taking a
        // temporary shared_ptr here could make its blocking destructor run on
        // the caller if the open worker releases its last reference meanwhile.
        if (websocket_ != nullptr) {
            websocket_->Interrupt();
        }
    }
    if (canceled != nullptr) {
        xEventGroupSetBits(canceled->event_group, WEBSOCKET_PROTOCOL_CANCEL_EVENT);
    }
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    std::shared_ptr<ConnectionAttempt> canceled;
    std::shared_ptr<WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        ++generation_;
        // Application runs Close after its queued Open on the same worker.
        cancel_next_open_ = false;
        canceled = std::move(attempt_);
        websocket = std::move(websocket_);
        opened_ = false;
    }
    if (canceled != nullptr) {
        xEventGroupSetBits(canceled->event_group, WEBSOCKET_PROTOCOL_CANCEL_EVENT);
    }
    if (websocket == nullptr) {
        return;
    }

    // The application runs physical close on its background worker. Waiting here
    // keeps the WebSocket destructor on that worker even if another task was sending.
    // Abort first: Tcp::Send has no deadline and may otherwise keep active_sends_
    // nonzero forever. Do not hold channel_mutex_ while joining TCP callbacks.
    websocket->Abort();
    std::unique_lock<std::mutex> lock(channel_mutex_);
    send_done_.wait(lock, [this]() { return active_sends_ == 0; });
    lock.unlock();
    websocket.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    std::shared_ptr<WebSocket> previous;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (opening_) {
            return false;
        }
        if (cancel_next_open_) {
            cancel_next_open_ = false;
            return false;
        }
        opening_ = true;
        generation = ++generation_;
        attempt_.reset();
        previous = std::move(websocket_);
        opened_ = false;
        error_occurred_ = false;
        session_id_.clear();
        // A hello may omit audio_params; do not reuse the previous session's values.
        server_sample_rate_ = 24000;
        server_frame_duration_ = 60;
    }

    std::shared_ptr<ConnectionAttempt> attempt;
    std::shared_ptr<WebSocket> websocket;
    bool success = false;
    auto finish_open = [this, &attempt, &websocket, &success]() {
        std::shared_ptr<WebSocket> failed_websocket;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            opening_ = false;
            if (!success && attempt_ == attempt) {
                attempt_.reset();
                opened_ = false;
                if (websocket_ == websocket) {
                    failed_websocket = std::move(websocket_);
                }
            }
        }
        failed_websocket.reset();
    };
    ScopeExit<decltype(finish_open)> open_guard(finish_open);
    // A previous sender may be blocked in Tcp::Send. Closing the old socket
    // releases it before waiting for active_sends_ below.
    if (previous != nullptr) {
        previous->Abort();
    }
    {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        send_done_.wait(lock, [this]() { return active_sends_ == 0; });
    }
    previous.reset();

    attempt = std::make_shared<ConnectionAttempt>(generation);
    if (attempt->event_group == nullptr) {
        ESP_LOGE(TAG, "Failed to create WebSocket hello event group");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (generation_ != generation) {
            return false;
        }
        attempt_ = attempt;
    }

    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected) {
            return false;
        }
        if (version != 0) {
            version_ = version;
        }
        attempt->version = version_;
        version = version_;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto created = network->CreateWebSocket(1);
    if (created == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }
    websocket = std::shared_ptr<WebSocket>(std::move(created));

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket->SetHeader("Authorization", token.c_str());
    }
    websocket->SetHeader("Protocol-Version", std::to_string(version).c_str());
    websocket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    std::weak_ptr<ConnectionAttempt> weak_attempt = attempt;
    websocket->OnData([this, weak_attempt](const char* data, size_t len, bool binary) {
        if (auto attempt = weak_attempt.lock()) {
            HandleData(attempt, data, len, binary);
        }
    });
    websocket->OnDisconnected([this, weak_attempt]() {
        if (auto attempt = weak_attempt.lock()) {
            HandleDisconnected(attempt);
        }
    });

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt)) {
            return false;
        }
        websocket_ = websocket;
    }

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version);
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected) {
            return false;
        }
    }
    if (auto connected = websocket->Connect(url.c_str()); !connected) {
        ESP_LOGE(TAG, "Failed to connect to websocket server: %s",
                 connected.error().ToString().c_str());
        ReportError(attempt, std::string(Lang::Strings::SERVER_NOT_CONNECTED) + "\n" + url);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected) {
            return false;
        }
    }

    const auto message = GetHelloMessage(version);
    bool sent;
    {
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (!IsCurrentLocked(attempt) || attempt->disconnected) {
                return false;
            }
        }
        sent = websocket->Send(message);
    }
    if (!sent) {
        ReportError(attempt, Lang::Strings::SERVER_ERROR);
        return false;
    }

    constexpr EventBits_t kWakeBits = WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT |
                                      WEBSOCKET_PROTOCOL_CANCEL_EVENT |
                                      WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT;
    xEventGroupWaitBits(attempt->event_group, kWakeBits, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    bool disconnected;
    bool hello_received;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt)) {
            return false;
        }
        disconnected = attempt->disconnected;
        hello_received = attempt->hello_received;
        if (hello_received && !disconnected) {
            opened_ = true;
        }
    }
    if (disconnected) {
        ReportError(attempt, Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    if (!hello_received) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        ReportError(attempt, Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        success = IsCurrentLocked(attempt) && !attempt->disconnected && opened_;
    }
    if (!success) {
        return false;
    }
    if (on_audio_channel_opened_ != nullptr) {
        // Cancellation may have arrived after the hello wakeup. The callback is
        // committed only while this attempt is still the current open channel.
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (!IsCurrentLocked(attempt) || attempt->disconnected || !opened_) {
                return false;
            }
        }
        on_audio_channel_opened_();
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        success = IsCurrentLocked(attempt) && !attempt->disconnected && opened_;
    }
    return success;
}

std::string WebsocketProtocol::GetHelloMessage(int version) {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::HandleData(const std::shared_ptr<ConnectionAttempt>& attempt,
                                   const char* data, size_t len, bool binary) {
    int version;
    int sample_rate;
    int frame_duration;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected) {
            return;
        }
        version = attempt->version;
        sample_rate = server_sample_rate_;
        frame_duration = server_frame_duration_;
    }

    if (binary) {
        if (on_incoming_audio_ == nullptr) {
            return;
        }
        std::unique_ptr<AudioStreamPacket> packet;
        if (version == 2) {
            if (len < sizeof(BinaryProtocol2)) {
                ESP_LOGW(TAG, "Discarding short WebSocket audio v2 frame: %zu", len);
                return;
            }
            const auto* bp2 = reinterpret_cast<const BinaryProtocol2*>(data);
            const size_t payload_size = ntohl(bp2->payload_size);
            if (ntohs(bp2->version) != 2 || ntohs(bp2->type) != 0 ||
                payload_size > len - sizeof(BinaryProtocol2)) {
                ESP_LOGW(TAG, "Discarding invalid WebSocket audio v2 frame: %zu", len);
                return;
            }
            const auto* payload = reinterpret_cast<const uint8_t*>(data) + sizeof(BinaryProtocol2);
            packet = std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                .sample_rate = sample_rate,
                .frame_duration = frame_duration,
                .timestamp = ntohl(bp2->timestamp),
                .payload = std::vector<uint8_t>(payload, payload + payload_size)});
        } else if (version == 3) {
            if (len < sizeof(BinaryProtocol3)) {
                ESP_LOGW(TAG, "Discarding short WebSocket audio v3 frame: %zu", len);
                return;
            }
            const auto* bp3 = reinterpret_cast<const BinaryProtocol3*>(data);
            const size_t payload_size = ntohs(bp3->payload_size);
            if (bp3->type != 0 || payload_size > len - sizeof(BinaryProtocol3)) {
                ESP_LOGW(TAG, "Discarding invalid WebSocket audio v3 frame: %zu", len);
                return;
            }
            const auto* payload = reinterpret_cast<const uint8_t*>(data) + sizeof(BinaryProtocol3);
            packet = std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                .sample_rate = sample_rate,
                .frame_duration = frame_duration,
                .timestamp = 0,
                .payload = std::vector<uint8_t>(payload, payload + payload_size)});
        } else {
            const auto* payload = reinterpret_cast<const uint8_t*>(data);
            packet = std::make_unique<AudioStreamPacket>(
                AudioStreamPacket{.sample_rate = sample_rate,
                                  .frame_duration = frame_duration,
                                  .timestamp = 0,
                                  .payload = std::vector<uint8_t>(payload, payload + len)});
        }
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (!IsCurrentLocked(attempt) || attempt->disconnected) {
                return;
            }
        }
        on_incoming_audio_(std::move(packet));
    } else {
        auto* root = cJSON_ParseWithLength(data, len);
        if (root == nullptr) {
            ESP_LOGW(TAG, "Discarding malformed WebSocket JSON frame: %zu", len);
            return;
        }
        auto* type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type)) {
            if (strcmp(type->valuestring, "hello") == 0) {
                ParseServerHello(attempt, root);
            } else if (on_incoming_json_ != nullptr) {
                bool current;
                {
                    std::lock_guard<std::mutex> lock(channel_mutex_);
                    current = IsCurrentLocked(attempt) && !attempt->disconnected;
                }
                if (current) {
                    on_incoming_json_(root);
                }
            }
        } else {
            ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
        }
        cJSON_Delete(root);
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (IsCurrentLocked(attempt) && !attempt->disconnected) {
        last_incoming_time_ = std::chrono::steady_clock::now();
    }
}

void WebsocketProtocol::HandleDisconnected(const std::shared_ptr<ConnectionAttempt>& attempt) {
    bool notify_closed;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected) {
            return;
        }
        attempt->disconnected = true;
        notify_closed = opened_;
        opened_ = false;
    }
    xEventGroupSetBits(attempt->event_group, WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT);
    ESP_LOGI(TAG, "Websocket disconnected");
    if (notify_closed && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void WebsocketProtocol::ReportError(const std::shared_ptr<ConnectionAttempt>& attempt,
                                    const std::string& message) {
    std::function<void(const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt)) {
            return;
        }
        error_occurred_ = true;
        opened_ = false;
        callback = on_network_error_;
    }
    if (callback != nullptr) {
        callback(message);
    }
}

void WebsocketProtocol::ParseServerHello(const std::shared_ptr<ConnectionAttempt>& attempt,
                                         const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport)) {
        ESP_LOGE(TAG, "Missing or non-string transport in server hello");
        return;
    }
    if (strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    const auto* session_id = cJSON_GetObjectItem(root, "session_id");

    const auto* audio_params = cJSON_GetObjectItem(root, "audio_params");
    const auto* sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
    const auto* frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
    std::string session;
    bool has_session = cJSON_IsString(session_id);
    if (has_session) {
        session = session_id->valuestring;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!IsCurrentLocked(attempt) || attempt->disconnected || attempt->hello_received) {
            return;
        }
        if (has_session) {
            session_id_ = session;
        }
        if (cJSON_IsObject(audio_params)) {
            if (cJSON_IsNumber(sample_rate)) {
                server_sample_rate_ = sample_rate->valueint;
            }
            if (cJSON_IsNumber(frame_duration)) {
                server_frame_duration_ = frame_duration->valueint;
            }
        }
        attempt->hello_received = true;
        last_incoming_time_ = std::chrono::steady_clock::now();
    }
    if (has_session) {
        ESP_LOGI(TAG, "Session ID: %s", session.c_str());
    }
    xEventGroupSetBits(attempt->event_group, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
