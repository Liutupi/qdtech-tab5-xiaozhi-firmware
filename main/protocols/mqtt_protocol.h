#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H

#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt.h>
#include <psa/crypto.h>
#include <udp.h>
#include "protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct MqttCallbackGate;

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define MQTT_PROTOCOL_CANCEL_OPEN_EVENT (1 << 1)

class MqttProtocol : public Protocol, public std::enable_shared_from_this<MqttProtocol> {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CancelOpen() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    enum class OpenPhase {
        Idle,
        ConnectingMqtt,
        AwaitingHello,
        HelloReceived,
        ConnectingUdp,
        Open
    };

    struct ServerHello {
        std::string session_id;
        std::string udp_server;
        std::string aes_key;
        std::string aes_nonce;
        int udp_port = 0;
        int sample_rate = 24000;
        int frame_duration = 60;
    };

    EventGroupHandle_t event_group_handle_ = nullptr;
    std::shared_ptr<MqttCallbackGate> callback_gate_;

    std::string publish_topic_;

    mutable std::mutex channel_mutex_;
    std::condition_variable channel_callbacks_idle_;
    uint32_t audio_deliveries_ = 0;
    uint32_t control_deliveries_ = 0;
    std::mutex crypto_mutex_;
    std::mutex mqtt_mutex_;
    std::mutex timer_mutex_;
    std::shared_ptr<Mqtt> mqtt_;
    std::atomic<uint64_t> mqtt_generation_{0};
    std::atomic<uint64_t> mqtt_connected_generation_{0};
    std::atomic<bool> mqtt_connecting_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> open_cancelled_{false};
    std::atomic<bool> channel_error_{false};
    std::atomic<int64_t> last_incoming_us_{0};
    // Shared so SendAudio() can hold a reference and call Send() without
    // holding channel_mutex_; the UDP receive callback (run on the modem's
    // AT event task) also needs that mutex, so holding it across a blocking
    // Send() would stall AT response parsing and cause spurious timeouts.
    std::shared_ptr<Udp> udp_;
    psa_key_id_t aes_key_id_ = PSA_KEY_ID_NULL;
    std::string aes_nonce_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    uint64_t next_attempt_id_ = 0;
    uint64_t active_attempt_id_ = 0;
    uint64_t audio_generation_ = 0;
    uint64_t close_requested_generation_ = 0;
    uint64_t cancel_generation_ = 0;
    uint64_t consumed_cancel_generation_ = 0;
    uint64_t attempt_cancel_generation_ = 0;
    OpenPhase open_phase_ = OpenPhase::Idle;
    bool mqtt_reconnect_required_ = false;
    std::optional<ServerHello> pending_hello_;
    std::string active_session_id_;
    std::string aborted_session_id_;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    uintptr_t reconnect_timer_id_ = 0;

    bool StartMqttClient(bool report_error = false);
    bool IsMqttConnected(const std::shared_ptr<Mqtt>& client, uint64_t generation) const;
    void ArmReconnectTimer(uint64_t mqtt_generation);
    void HandleMqttMessage(uint64_t mqtt_generation, const std::string& payload);
    void ParseServerHello(const cJSON* root, uint64_t mqtt_generation);
    void QueueAudioClose(uint64_t audio_generation, std::string session_id);
    bool DecodeHexString(const std::string& hex_string, std::string& decoded);
    bool CryptAesCtrLocked(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                           uint8_t* output);
    bool IsAttemptActive(uint64_t attempt_id) const;
    void FinishAttempt(uint64_t attempt_id);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    using Protocol::SetError;
    void SetError(const std::string& message) override;
    bool IsTimeout() const override;
};

#endif  // MQTT_PROTOCOL_H
