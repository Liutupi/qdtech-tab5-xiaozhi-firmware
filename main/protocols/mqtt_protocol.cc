#include "mqtt_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <system_error>
#include <unordered_map>
#include "assets/lang_config.h"

#define TAG "MQTT"

struct MqttCallbackGate {
    std::mutex mutex;
    std::condition_variable idle;
    MqttProtocol* owner = nullptr;
    size_t active = 0;
    bool closing = false;
};

namespace {
class MqttCallbackLease {
public:
    explicit MqttCallbackLease(std::shared_ptr<MqttCallbackGate> gate) : gate_(std::move(gate)) {
        std::lock_guard<std::mutex> lock(gate_->mutex);
        if (!gate_->closing && gate_->owner != nullptr) {
            owner_ = gate_->owner;
            ++gate_->active;
        }
    }

    MqttCallbackLease(const MqttCallbackLease&) = delete;
    MqttCallbackLease& operator=(const MqttCallbackLease&) = delete;

    ~MqttCallbackLease() {
        if (!owner_) {
            return;
        }
        std::lock_guard<std::mutex> lock(gate_->mutex);
        if (--gate_->active == 0) {
            gate_->idle.notify_all();
        }
    }

    MqttProtocol* get() const { return owner_; }

private:
    std::shared_ptr<MqttCallbackGate> gate_;
    MqttProtocol* owner_ = nullptr;
};

// A timer callback may already be dispatched when esp_timer_stop/delete runs.
// Passing an integer token instead of a protocol pointer keeps that callback
// from dereferencing an object whose destructor has started.
std::mutex reconnect_registry_mutex;
std::unordered_map<uintptr_t, std::weak_ptr<MqttProtocol>> reconnect_registry;
std::atomic<uintptr_t> next_reconnect_timer_id{1};
}  // namespace

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();
    callback_gate_ = std::make_shared<MqttCallbackGate>();
    callback_gate_->owner = this;
    reconnect_timer_id_ = next_reconnect_timer_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(reconnect_registry_mutex);
        reconnect_registry.emplace(reconnect_timer_id_, std::weak_ptr<MqttProtocol>{});
    }

    esp_timer_create_args_t reconnect_timer_args = {
        .callback =
            [](void* arg) {
                const auto id = reinterpret_cast<uintptr_t>(arg);
                std::weak_ptr<MqttProtocol> weak;
                {
                    std::lock_guard<std::mutex> lock(reconnect_registry_mutex);
                    auto it = reconnect_registry.find(id);
                    if (it == reconnect_registry.end()) {
                        return;
                    }
                    weak = it->second;
                }
                // Enter through the main task so the no-background-worker
                // fallback cannot release the final Protocol reference from
                // this esp_timer callback's own stack.
                Application::GetInstance().Schedule([weak]() {
                    Application::GetInstance().ScheduleProtocolMaintenance(
                        [weak](Protocol& current) {
                            auto protocol = weak.lock();
                            if (protocol && &current == protocol.get() &&
                                !protocol->shutting_down_.load()) {
                                ESP_LOGI(TAG, "Reconnecting to MQTT server");
                                protocol->StartMqttClient(false);
                            }
                        });
                });
            },
        .arg = reinterpret_cast<void*>(reconnect_timer_id_),
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    shutting_down_.store(true);
    CancelOpen();
    {
        std::unique_lock<std::mutex> lock(callback_gate_->mutex);
        callback_gate_->closing = true;
        callback_gate_->owner = nullptr;
        callback_gate_->idle.wait(lock, [this]() { return callback_gate_->active == 0; });
    }
    {
        std::lock_guard<std::mutex> lock(reconnect_registry_mutex);
        reconnect_registry.erase(reconnect_timer_id_);
    }

    std::shared_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(mqtt_mutex_);
        mqtt_generation_.fetch_add(1);
        mqtt = std::move(mqtt_);
    }
    // Destroy the client before deleting the timer: its disconnect callback
    // must never attempt to arm a deleted timer.
    mqtt.reset();

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (reconnect_timer_ != nullptr) {
            esp_timer_stop(reconnect_timer_);
            esp_timer_delete(reconnect_timer_);
            reconnect_timer_ = nullptr;
        }
    }

    std::shared_ptr<Udp> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
    }
    udp.reset();

    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (aes_key_id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(aes_key_id_);
            aes_key_id_ = PSA_KEY_ID_NULL;
        }
    }

    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    {
        std::lock_guard<std::mutex> lock(reconnect_registry_mutex);
        reconnect_registry[reconnect_timer_id_] = weak_from_this();
    }
    return StartMqttClient(false);
}

bool MqttProtocol::IsMqttConnected(const std::shared_ptr<Mqtt>& client, uint64_t generation) const {
    return client && mqtt_generation_.load() == generation &&
           mqtt_connected_generation_.load() == generation;
}

void MqttProtocol::ArmReconnectTimer(uint64_t generation) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (shutting_down_.load() || mqtt_generation_.load() != generation ||
        mqtt_connected_generation_.load() == generation || reconnect_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_periodic(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (shutting_down_.load()) {
        return false;
    }
    if (!report_error) {
        std::shared_ptr<Mqtt> current;
        uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(mqtt_mutex_);
            current = mqtt_;
            generation = mqtt_generation_.load();
        }
        if (IsMqttConnected(current, generation)) {
            return true;
        }
    }
    bool expected = false;
    if (!mqtt_connecting_.compare_exchange_strong(expected, true)) {
        return false;
    }
    struct ConnectGuard {
        std::atomic<bool>& connecting;
        ~ConnectGuard() { connecting.store(false); }
    } connect_guard{mqtt_connecting_};

    std::shared_ptr<Mqtt> previous;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mqtt_mutex_);
        generation = mqtt_generation_.fetch_add(1) + 1;
        previous = std::move(mqtt_);
        publish_topic_.clear();
    }
    previous.reset();

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    auto publish_topic = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        ArmReconnectTimer(generation);
        if (report_error && !open_cancelled_.load()) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    std::shared_ptr<Mqtt> mqtt(network->CreateMqtt(0));
    if (!mqtt || shutting_down_.load() || mqtt_generation_.load() != generation) {
        ArmReconnectTimer(generation);
        return false;
    }
    mqtt->SetKeepAlive(keepalive_interval);
    auto gate = callback_gate_;

    mqtt->OnDisconnected([gate, generation]() {
        MqttCallbackLease lease(gate);
        auto* self = lease.get();
        if (!self) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(self->mqtt_mutex_);
            if (self->shutting_down_.load() || self->mqtt_generation_.load() != generation) {
                return;
            }
            uint64_t connected_generation = generation;
            self->mqtt_connected_generation_.compare_exchange_strong(connected_generation, 0);
        }
        uint64_t close_generation = 0;
        std::string close_session;
        bool cancel_pending = false;
        {
            std::lock_guard<std::mutex> lock(self->channel_mutex_);
            if (self->mqtt_generation_.load() != generation) {
                return;
            }
            if (self->open_phase_ == OpenPhase::Open && self->udp_) {
                if (self->close_requested_generation_ != self->audio_generation_) {
                    self->open_cancelled_.store(true);
                    close_generation = self->audio_generation_;
                    self->close_requested_generation_ = close_generation;
                    close_session = self->active_session_id_;
                }
            } else if (self->open_phase_ != OpenPhase::Idle) {
                self->attempt_cancel_generation_ = ++self->cancel_generation_;
                self->open_cancelled_.store(true);
                self->mqtt_reconnect_required_ = true;
                self->active_attempt_id_ = 0;
                self->open_phase_ = OpenPhase::Idle;
                self->pending_hello_.reset();
                self->aborted_session_id_.clear();
                ++self->audio_generation_;
                cancel_pending = true;
            }
        }
        if (cancel_pending) {
            xEventGroupSetBits(self->event_group_handle_, MQTT_PROTOCOL_CANCEL_OPEN_EVENT);
        }
        if (close_generation != 0) {
            self->QueueAudioClose(close_generation, std::move(close_session));
        }
        if (self->on_disconnected_ != nullptr) {
            self->on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, retry reconnect every %d seconds",
                 MQTT_RECONNECT_INTERVAL_MS / 1000);
        self->ArmReconnectTimer(generation);
    });

    mqtt->OnConnected([gate, generation]() {
        MqttCallbackLease lease(gate);
        auto* self = lease.get();
        if (!self) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(self->mqtt_mutex_);
            if (self->shutting_down_.load() || self->mqtt_generation_.load() != generation) {
                return;
            }
            self->mqtt_connected_generation_.store(generation);
        }
        if (self->on_connected_ != nullptr) {
            self->on_connected_();
        }
        std::lock_guard<std::mutex> lock(self->timer_mutex_);
        if (!self->shutting_down_.load() && self->mqtt_generation_.load() == generation &&
            self->mqtt_connected_generation_.load() == generation &&
            self->reconnect_timer_ != nullptr) {
            esp_timer_stop(self->reconnect_timer_);
        }
    });

    mqtt->OnMessage([gate, generation](const std::string&, const std::string& payload) {
        MqttCallbackLease lease(gate);
        if (auto* self = lease.get()) {
            self->HandleMqttMessage(generation, payload);
        }
    });

    {
        std::lock_guard<std::mutex> lock(mqtt_mutex_);
        if (shutting_down_.load() || mqtt_generation_.load() != generation) {
            return false;
        }
        mqtt_ = mqtt;
        publish_topic_ = std::move(publish_topic);
    }

    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        auto port_str = endpoint.substr(pos + 1);
        int parsed_port = 0;
        auto [ptr, ec] =
            std::from_chars(port_str.data(), port_str.data() + port_str.size(), parsed_port);
        if (ec == std::errc() && ptr == port_str.data() + port_str.size() && parsed_port > 0 &&
            parsed_port <= UINT16_MAX) {
            broker_port = parsed_port;
        } else {
            ESP_LOGW(TAG, "Invalid port in MQTT endpoint \"%s\", using %d", endpoint.c_str(),
                     broker_port);
        }
    } else {
        broker_address = endpoint;
    }
    ESP_LOGI(TAG, "Connecting to endpoint %s:%d", broker_address.c_str(), broker_port);
    if (Board::GetInstance().GetBoardType() == "wifi") {
        struct hostent* server = gethostbyname(broker_address.c_str());
        if (server != nullptr && server->h_addr != nullptr) {
            ESP_LOGI(TAG, "Resolved MQTT host %s -> %s", broker_address.c_str(),
                     inet_ntoa(*reinterpret_cast<struct in_addr*>(server->h_addr)));
        } else {
            ESP_LOGW(TAG, "Failed to resolve MQTT host %s", broker_address.c_str());
        }
    }
    if (auto connected = mqtt->Connect(broker_address, broker_port, client_id, username, password);
        !connected) {
        ESP_LOGE(TAG, "Failed to connect to endpoint: %s", connected.error().ToString().c_str());
        ArmReconnectTimer(generation);
        if (report_error && !shutting_down_.load() && !open_cancelled_.load() &&
            mqtt_generation_.load() == generation) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED,
                     broker_address + ":" + std::to_string(broker_port));
        }
        return false;
    }

    if (shutting_down_.load() || mqtt_generation_.load() != generation) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (reconnect_timer_ != nullptr && mqtt_generation_.load() == generation &&
            mqtt_connected_generation_.load() == generation) {
            esp_timer_stop(reconnect_timer_);
        }
    }
    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    std::shared_ptr<Mqtt> mqtt;
    std::string topic;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mqtt_mutex_);
        mqtt = mqtt_;
        topic = publish_topic_;
        generation = mqtt_generation_.load();
    }
    if (!mqtt || topic.empty() || shutting_down_.load()) {
        return false;
    }
    if (!mqtt->Publish(topic, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        if (!shutting_down_.load() && !open_cancelled_.load() &&
            mqtt_generation_.load() == generation) {
            SetError(Lang::Strings::SERVER_ERROR);
        }
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    constexpr size_t kAudioHeaderSize = 16;
    if (!packet || packet->payload.empty() || packet->payload.size() > UINT16_MAX) {
        return false;
    }

    // Keep the critical section short: only snapshot the channel state here.
    // udp->Send() below may block waiting for the modem, and the UDP receive
    // callback needs channel_mutex_ on the modem's AT event task.
    std::shared_ptr<Udp> udp;
    std::string nonce;
    uint32_t sequence;
    std::string encrypted;
    encrypted.resize(kAudioHeaderSize + packet->payload.size());
    std::unique_lock<std::mutex> crypto_lock(crypto_mutex_);
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (udp_ == nullptr || open_phase_ != OpenPhase::Open || open_cancelled_.load()) {
            return false;
        }
        if (aes_nonce_.size() != kAudioHeaderSize) {
            ESP_LOGE(TAG, "Invalid AES nonce or audio payload length: %zu", packet->payload.size());
            return false;
        }
        udp = udp_;
        nonce = aes_nonce_;
        sequence = htonl(++local_sequence_);
    }

    const uint16_t payload_len = htons(static_cast<uint16_t>(packet->payload.size()));
    const uint32_t timestamp = htonl(packet->timestamp);
    memcpy(nonce.data() + 2, &payload_len, sizeof(payload_len));
    memcpy(nonce.data() + 8, &timestamp, sizeof(timestamp));
    memcpy(nonce.data() + 12, &sequence, sizeof(sequence));

    memcpy(encrypted.data(), nonce.data(), nonce.size());

    if (!CryptAesCtrLocked(reinterpret_cast<const uint8_t*>(packet->payload.data()),
                           packet->payload.size(), reinterpret_cast<const uint8_t*>(nonce.data()),
                           reinterpret_cast<uint8_t*>(&encrypted[nonce.size()]))) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }
    crypto_lock.unlock();

    // Send without holding channel_mutex_ (see above).
    return udp->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    CancelOpen();
    std::shared_ptr<Udp> udp;
    std::string session_id;
    bool was_open;
    uint64_t close_cancel_generation;
    {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        close_cancel_generation = cancel_generation_;
        udp = std::move(udp_);
        was_open = udp != nullptr;
        session_id = std::move(active_session_id_);
        if (session_id.empty()) {
            session_id = std::move(aborted_session_id_);
        } else {
            aborted_session_id_.clear();
        }
        session_id_.clear();
        pending_hello_.reset();
        active_attempt_id_ = 0;
        open_phase_ = OpenPhase::Idle;
        ++audio_generation_;
        channel_callbacks_idle_.wait(
            lock, [this]() { return audio_deliveries_ == 0 && control_deliveries_ == 0; });
    }
    udp.reset();

    ESP_LOGI(TAG, "Closing audio channel, send_goodbye: %d", send_goodbye);

    if (send_goodbye && !session_id.empty()) {
        cJSON* goodbye = cJSON_CreateObject();
        if (goodbye != nullptr) {
            if (cJSON_AddStringToObject(goodbye, "session_id", session_id.c_str()) != nullptr &&
                cJSON_AddStringToObject(goodbye, "type", "goodbye") != nullptr) {
                char* message = cJSON_PrintUnformatted(goodbye);
                if (message != nullptr) {
                    SendText(message);
                    cJSON_free(message);
                }
            }
            cJSON_Delete(goodbye);
        }
    }

    if (was_open && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (close_cancel_generation > consumed_cancel_generation_) {
            consumed_cancel_generation_ = close_cancel_generation;
        }
        open_cancelled_.store(cancel_generation_ > consumed_cancel_generation_);
    }
}

void MqttProtocol::CancelOpen() {
    bool had_pending_open = false;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        ++cancel_generation_;
        open_cancelled_.store(true);
        if (open_phase_ != OpenPhase::Idle && open_phase_ != OpenPhase::Open) {
            attempt_cancel_generation_ = cancel_generation_;
            if (open_phase_ == OpenPhase::AwaitingHello ||
                open_phase_ == OpenPhase::HelloReceived ||
                open_phase_ == OpenPhase::ConnectingUdp) {
                mqtt_reconnect_required_ = true;
            }
            if (pending_hello_) {
                aborted_session_id_ = pending_hello_->session_id;
            }
            active_attempt_id_ = 0;
            open_phase_ = OpenPhase::Idle;
            pending_hello_.reset();
            ++audio_generation_;
            had_pending_open = true;
        }
    }
    if (had_pending_open && event_group_handle_ != nullptr) {
        xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_CANCEL_OPEN_EVENT);
    }
}

bool MqttProtocol::IsAttemptActive(uint64_t attempt_id) const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return active_attempt_id_ == attempt_id && open_phase_ != OpenPhase::Idle &&
           !open_cancelled_.load() && !shutting_down_.load();
}

void MqttProtocol::FinishAttempt(uint64_t attempt_id) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (active_attempt_id_ == attempt_id && open_phase_ != OpenPhase::Open) {
        if (open_phase_ == OpenPhase::AwaitingHello || open_phase_ == OpenPhase::HelloReceived ||
            open_phase_ == OpenPhase::ConnectingUdp) {
            mqtt_reconnect_required_ = true;
        }
        active_attempt_id_ = 0;
        open_phase_ = OpenPhase::Idle;
        pending_hello_.reset();
    }
    if (active_attempt_id_ == 0 && open_phase_ == OpenPhase::Idle) {
        if (attempt_cancel_generation_ > consumed_cancel_generation_) {
            consumed_cancel_generation_ = attempt_cancel_generation_;
        }
        open_cancelled_.store(cancel_generation_ > consumed_cancel_generation_);
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (event_group_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MQTT handshake event group");
        return false;
    }
    std::shared_ptr<Udp> stale_udp;
    uint64_t attempt_id;
    uint64_t audio_generation;
    bool reconnect_mqtt;
    {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        // CancelOpen may arrive after Application's final epoch check but
        // before this worker enters OpenAudioChannel. Consume that cancellation.
        if (cancel_generation_ > consumed_cancel_generation_) {
            consumed_cancel_generation_ = cancel_generation_;
            open_cancelled_.store(false);
            return false;
        }
        if (open_phase_ != OpenPhase::Idle && open_phase_ != OpenPhase::Open) {
            ESP_LOGW(TAG, "Audio channel open already in progress");
            return false;
        }
        stale_udp = std::move(udp_);
        reconnect_mqtt = mqtt_reconnect_required_;
        mqtt_reconnect_required_ = false;
        active_session_id_.clear();
        aborted_session_id_.clear();
        session_id_.clear();
        pending_hello_.reset();
        active_attempt_id_ = attempt_id = ++next_attempt_id_;
        audio_generation = ++audio_generation_;
        open_phase_ = OpenPhase::ConnectingMqtt;
        open_cancelled_.store(false);
        channel_error_.store(false);
        last_incoming_us_.store(esp_timer_get_time());
        xEventGroupClearBits(event_group_handle_,
                             MQTT_PROTOCOL_SERVER_HELLO_EVENT | MQTT_PROTOCOL_CANCEL_OPEN_EVENT);
        channel_callbacks_idle_.wait(
            lock, [this]() { return audio_deliveries_ == 0 && control_deliveries_ == 0; });
    }
    struct AttemptCleanup {
        MqttProtocol* protocol;
        uint64_t attempt_id;
        ~AttemptCleanup() { protocol->FinishAttempt(attempt_id); }
    } cleanup{this, attempt_id};
    stale_udp.reset();

    std::shared_ptr<Mqtt> mqtt;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mqtt_mutex_);
        mqtt = mqtt_;
        generation = mqtt_generation_.load();
    }
    if (reconnect_mqtt || !IsMqttConnected(mqtt, generation)) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            FinishAttempt(attempt_id);
            return false;
        }
    }
    if (!IsAttemptActive(attempt_id)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (active_attempt_id_ != attempt_id || open_cancelled_.load()) {
            return false;
        }
        open_phase_ = OpenPhase::AwaitingHello;
    }

    auto message = GetHelloMessage();
    if (message.empty() || !SendText(message)) {
        FinishAttempt(attempt_id);
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT | MQTT_PROTOCOL_CANCEL_OPEN_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!IsAttemptActive(attempt_id) || (bits & MQTT_PROTOCOL_CANCEL_OPEN_EVENT)) {
        FinishAttempt(attempt_id);
        return false;
    }
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        FinishAttempt(attempt_id);
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    ServerHello hello;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (active_attempt_id_ != attempt_id || open_phase_ != OpenPhase::HelloReceived ||
            !pending_hello_ || open_cancelled_.load()) {
            return false;
        }
        hello = *pending_hello_;
        open_phase_ = OpenPhase::ConnectingUdp;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto udp = network->CreateUdp(2);
    if (!udp) {
        FinishAttempt(attempt_id);
        return false;
    }
    auto gate = callback_gate_;
    udp->OnMessage([gate, audio_generation](const std::string& data) {
        MqttCallbackLease lease(gate);
        auto* self = lease.get();
        if (!self) {
            return;
        }
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        constexpr size_t kAudioHeaderSize = 16;
        if (data.size() < kAudioHeaderSize) {
            ESP_LOGE(TAG, "Invalid audio packet size: %zu", data.size());
            return;
        }
        if (static_cast<uint8_t>(data[0]) != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", static_cast<uint8_t>(data[0]));
            return;
        }
        uint16_t payload_len = 0;
        uint32_t timestamp = 0;
        uint32_t sequence = 0;
        memcpy(&payload_len, data.data() + 2, sizeof(payload_len));
        memcpy(&timestamp, data.data() + 8, sizeof(timestamp));
        memcpy(&sequence, data.data() + 12, sizeof(sequence));
        payload_len = ntohs(payload_len);
        timestamp = ntohl(timestamp);
        sequence = ntohl(sequence);
        if (data.size() != kAudioHeaderSize + payload_len) {
            ESP_LOGE(TAG, "Audio payload length mismatch: header=%u, datagram=%zu",
                     static_cast<unsigned>(payload_len), data.size() - kAudioHeaderSize);
            return;
        }

        int sample_rate;
        int frame_duration;
        {
            std::lock_guard<std::mutex> lock(self->channel_mutex_);
            if (self->audio_generation_ != audio_generation ||
                self->open_phase_ != OpenPhase::Open || self->open_cancelled_.load()) {
                return;
            }
            if (sequence <= self->remote_sequence_) {
                ESP_LOGW(TAG, "Received duplicate/old audio sequence: %lu, last: %lu",
                         static_cast<unsigned long>(sequence),
                         static_cast<unsigned long>(self->remote_sequence_));
                return;
            }
            if (sequence != self->remote_sequence_ + 1) {
                ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu",
                         static_cast<unsigned long>(sequence),
                         static_cast<unsigned long>(self->remote_sequence_ + 1));
            }
            sample_rate = self->server_sample_rate_;
            frame_duration = self->server_frame_duration_;
        }

        const size_t decrypted_size = payload_len;
        auto nonce = reinterpret_cast<const uint8_t*>(data.data());
        auto encrypted = reinterpret_cast<const uint8_t*>(data.data() + kAudioHeaderSize);
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        {
            std::lock_guard<std::mutex> crypto_lock(self->crypto_mutex_);
            {
                std::lock_guard<std::mutex> channel_lock(self->channel_mutex_);
                if (self->audio_generation_ != audio_generation ||
                    self->open_phase_ != OpenPhase::Open || self->open_cancelled_.load()) {
                    return;
                }
            }
            if (!self->CryptAesCtrLocked(encrypted, decrypted_size, nonce,
                                         reinterpret_cast<uint8_t*>(packet->payload.data()))) {
                ESP_LOGE(TAG, "Failed to decrypt audio data");
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(self->channel_mutex_);
            if (self->audio_generation_ != audio_generation ||
                self->open_phase_ != OpenPhase::Open || self->open_cancelled_.load() ||
                sequence <= self->remote_sequence_) {
                return;
            }
            self->remote_sequence_ = sequence;
            ++self->audio_deliveries_;
        }
        self->last_incoming_us_.store(esp_timer_get_time());
        if (self->on_incoming_audio_ != nullptr) {
            self->on_incoming_audio_(std::move(packet));
        }
        {
            std::lock_guard<std::mutex> lock(self->channel_mutex_);
            if (--self->audio_deliveries_ == 0 && self->control_deliveries_ == 0) {
                self->channel_callbacks_idle_.notify_all();
            }
        }
    });

    if (!IsAttemptActive(attempt_id)) {
        return false;
    }
    if (auto connected = udp->Connect(hello.udp_server, hello.udp_port); !connected) {
        ESP_LOGE(TAG, "Failed to connect UDP audio channel: %s",
                 connected.error().ToString().c_str());
        FinishAttempt(attempt_id);
        return false;
    }
    if (!IsAttemptActive(attempt_id)) {
        return false;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Crypto, status: %ld", static_cast<long>(status));
        FinishAttempt(attempt_id);
        return false;
    }
    psa_key_id_t new_key = PSA_KEY_ID_NULL;
    {
        std::lock_guard<std::mutex> crypto_lock(crypto_mutex_);
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        status = psa_import_key(&attributes, reinterpret_cast<const uint8_t*>(hello.aes_key.data()),
                                hello.aes_key.size(), &new_key);
        psa_reset_key_attributes(&attributes);
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to import AES key, status: %ld", static_cast<long>(status));
        FinishAttempt(attempt_id);
        return false;
    }

    bool committed = false;
    {
        std::lock_guard<std::mutex> crypto_lock(crypto_mutex_);
        std::lock_guard<std::mutex> channel_lock(channel_mutex_);
        if (active_attempt_id_ == attempt_id && open_phase_ == OpenPhase::ConnectingUdp &&
            !open_cancelled_.load() && !shutting_down_.load()) {
            if (aes_key_id_ != PSA_KEY_ID_NULL) {
                psa_destroy_key(aes_key_id_);
            }
            aes_key_id_ = new_key;
            new_key = PSA_KEY_ID_NULL;
            aes_nonce_ = std::move(hello.aes_nonce);
            active_session_id_ = hello.session_id;
            session_id_ = hello.session_id;
            server_sample_rate_ = hello.sample_rate;
            server_frame_duration_ = hello.frame_duration;
            local_sequence_ = 0;
            remote_sequence_ = 0;
            udp_ = std::move(udp);
            pending_hello_.reset();
            open_phase_ = OpenPhase::Open;
            last_incoming_us_.store(esp_timer_get_time());
            committed = true;
        }
    }
    if (new_key != PSA_KEY_ID_NULL) {
        std::lock_guard<std::mutex> crypto_lock(crypto_mutex_);
        psa_destroy_key(new_key);
    }
    if (!committed) {
        return false;
    }

    if (IsAttemptActive(attempt_id) && on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return IsAttemptActive(attempt_id);
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
    if (features == nullptr) {
        cJSON_Delete(root);
        return {};
    }
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON* audio_params = cJSON_CreateObject();
    if (audio_params == nullptr) {
        cJSON_Delete(root);
        return {};
    }
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message;
    if (json_str != nullptr) {
        message = json_str;
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::QueueAudioClose(uint64_t audio_generation, std::string session_id) {
    auto weak = weak_from_this();
    Application::GetInstance().CloseAudioChannelAsync(
        false, [weak, audio_generation, session_id = std::move(session_id)](Protocol& current) {
            auto protocol = weak.lock();
            if (!protocol || &current != protocol.get()) {
                return false;
            }
            std::lock_guard<std::mutex> lock(protocol->channel_mutex_);
            return protocol->open_phase_ == OpenPhase::Open && protocol->udp_ &&
                   protocol->audio_generation_ == audio_generation &&
                   protocol->active_session_id_ == session_id;
        });
}

void MqttProtocol::HandleMqttMessage(uint64_t mqtt_generation, const std::string& payload) {
    if (shutting_down_.load() || mqtt_generation_.load() != mqtt_generation) {
        return;
    }
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
        return;
    }
    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGE(TAG, "Message type is invalid");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        ParseServerHello(root, mqtt_generation);
    } else if (strcmp(type->valuestring, "goodbye") == 0) {
        auto session_id = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(session_id) && session_id->valuestring[0] != '\0') {
            const std::string goodbye_session = session_id->valuestring;
            uint64_t expected_audio_generation = 0;
            bool cancel_pending = false;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                if (mqtt_generation_.load() == mqtt_generation && open_phase_ == OpenPhase::Open &&
                    udp_ && active_session_id_ == goodbye_session &&
                    close_requested_generation_ != audio_generation_) {
                    expected_audio_generation = audio_generation_;
                    close_requested_generation_ = expected_audio_generation;
                    open_cancelled_.store(true);
                } else if (mqtt_generation_.load() == mqtt_generation && pending_hello_ &&
                           pending_hello_->session_id == goodbye_session &&
                           open_phase_ != OpenPhase::Idle && open_phase_ != OpenPhase::Open) {
                    attempt_cancel_generation_ = ++cancel_generation_;
                    open_cancelled_.store(true);
                    mqtt_reconnect_required_ = true;
                    active_attempt_id_ = 0;
                    open_phase_ = OpenPhase::Idle;
                    pending_hello_.reset();
                    aborted_session_id_.clear();
                    ++audio_generation_;
                    cancel_pending = true;
                }
            }
            if (cancel_pending) {
                xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_CANCEL_OPEN_EVENT);
            } else if (expected_audio_generation != 0) {
                QueueAudioClose(expected_audio_generation, goodbye_session);
            }
        }
    } else {
        // Notifications are connection-level messages that intentionally have
        // no session_id and may arrive while Idle. Other control messages are
        // bound to the current audio session. Count active deliveries so a
        // Close/Open cannot let an old callback run into a replacement session.
        auto session_id = cJSON_GetObjectItem(root, "session_id");
        bool deliver = false;
        bool session_activity = false;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (!open_cancelled_.load() && mqtt_generation_.load() == mqtt_generation &&
                on_incoming_json_ != nullptr) {
                if (strcmp(type->valuestring, "notify") == 0 && session_id == nullptr) {
                    deliver = true;
                } else if (cJSON_IsString(session_id) &&
                           active_session_id_ == session_id->valuestring &&
                           open_phase_ == OpenPhase::Open && udp_ != nullptr) {
                    deliver = true;
                    session_activity = true;
                }
                if (deliver) {
                    ++control_deliveries_;
                }
            }
        }
        if (deliver) {
            if (session_activity) {
                last_incoming_us_.store(esp_timer_get_time());
            }
            on_incoming_json_(root);
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (--control_deliveries_ == 0 && audio_deliveries_ == 0) {
                channel_callbacks_idle_.notify_all();
            }
        }
    }
    cJSON_Delete(root);
}

void MqttProtocol::ParseServerHello(const cJSON* root, uint64_t mqtt_generation) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported or missing transport");
        return;
    }

    ServerHello hello;
    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (!cJSON_IsString(session_id) || session_id->valuestring[0] == '\0') {
        ESP_LOGE(TAG, "Missing MQTT session ID");
        return;
    }
    hello.session_id = session_id->valuestring;

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate) && sample_rate->valueint > 0) {
            hello.sample_rate = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration) && frame_duration->valueint > 0) {
            hello.frame_duration = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    auto server = cJSON_GetObjectItem(udp, "server");
    auto port = cJSON_GetObjectItem(udp, "port");
    auto key_item = cJSON_GetObjectItem(udp, "key");
    auto nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || !cJSON_IsNumber(port) || port->valueint <= 0 ||
        port->valueint > UINT16_MAX || !cJSON_IsString(key_item) || !cJSON_IsString(nonce_item)) {
        ESP_LOGE(TAG, "Invalid UDP server, port, key, or nonce");
        return;
    }
    hello.udp_server = server->valuestring;
    hello.udp_port = port->valueint;
    if (!DecodeHexString(nonce_item->valuestring, hello.aes_nonce) ||
        !DecodeHexString(key_item->valuestring, hello.aes_key) || hello.aes_nonce.size() != 16 ||
        hello.aes_key.size() != 16) {
        ESP_LOGE(TAG, "Invalid AES key or nonce length");
        return;
    }
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!shutting_down_.load() && !open_cancelled_.load() &&
            mqtt_generation_.load() == mqtt_generation && open_phase_ == OpenPhase::AwaitingHello) {
            pending_hello_ = std::move(hello);
            open_phase_ = OpenPhase::HelloReceived;
            accepted = true;
        }
    }
    if (accepted) {
        xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
    }
}

bool MqttProtocol::CryptAesCtrLocked(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                                     uint8_t* output) {
    if (aes_key_id_ == PSA_KEY_ID_NULL || input == nullptr || nonce == nullptr ||
        output == nullptr) {
        return false;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = psa_cipher_encrypt_setup(&operation, aes_key_id_, PSA_ALG_CTR);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, nonce, 16);
    }

    size_t output_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, input, input_size, output, input_size, &output_len);
    }

    uint8_t finish_output[16];
    size_t finish_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, finish_output, sizeof(finish_output), &finish_len);
    }
    psa_cipher_abort(&operation);

    if (status != PSA_SUCCESS || output_len != input_size || finish_len != 0) {
        ESP_LOGE(TAG, "AES-CTR operation failed, status: %ld", static_cast<long>(status));
        return false;
    }
    return true;
}

static inline int CharToHex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool MqttProtocol::DecodeHexString(const std::string& hex_string, std::string& decoded) {
    decoded.clear();
    if ((hex_string.size() % 2) != 0) {
        return false;
    }
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        int high = CharToHex(hex_string[i]);
        int low = CharToHex(hex_string[i + 1]);
        if (high < 0 || low < 0) {
            decoded.clear();
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return udp_ != nullptr && open_phase_ == OpenPhase::Open && !open_cancelled_.load() &&
           !channel_error_.load() && !IsTimeout();
}

void MqttProtocol::SetError(const std::string& message) {
    if (shutting_down_.load() || open_cancelled_.load()) {
        return;
    }
    channel_error_.store(true);
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

bool MqttProtocol::IsTimeout() const {
    const int64_t last = last_incoming_us_.load();
    if (last == 0) {
        return false;
    }
    const int64_t elapsed_us = esp_timer_get_time() - last;
    if (elapsed_us > 120LL * 1000000) {
        ESP_LOGE(TAG, "Channel timeout %lld seconds", static_cast<long long>(elapsed_us / 1000000));
        return true;
    }
    return false;
}
