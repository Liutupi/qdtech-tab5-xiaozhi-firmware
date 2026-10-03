#include "esp_mqtt.h"
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include <utility>

static const char* TAG = "esp_mqtt";

namespace {
// Match the WebSocket receive budget: large MCP lyrics need external malloc,
// while boards with only internal RAM must keep one JSON message small.
#if defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_USE_MALLOC
constexpr size_t kMaxMessageBytes = 128 * 1024;
#else
constexpr size_t kMaxMessageBytes = 16 * 1024;
#endif
constexpr int kMaxTopicBytes = 512;
}  // namespace

EspMqtt::EspMqtt() { event_group_handle_ = xEventGroupCreate(); }

EspMqtt::~EspMqtt() {
    Disconnect();
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

NetworkResult<> EspMqtt::Connect(const std::string broker_address, int broker_port,
                                 const std::string client_id, const std::string username,
                                 const std::string password) {
    if (mqtt_client_handle_ != nullptr) {
        Disconnect();
    }
    ResetMessageAssembly();
    connected_.store(false);

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.task.stack_size = 4096;
    mqtt_config.broker.address.hostname = broker_address.c_str();
    mqtt_config.broker.address.port = broker_port;
    if (broker_port == 8883) {
        mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
        mqtt_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    }
    mqtt_config.credentials.client_id = client_id.c_str();
    mqtt_config.credentials.username = username.c_str();
    mqtt_config.credentials.authentication.password = password.c_str();
    mqtt_config.session.keepalive = keep_alive_seconds_;

    mqtt_client_handle_ = esp_mqtt_client_init(&mqtt_config);
    if (mqtt_client_handle_ == nullptr) {
        return Fail(NetworkError::ConnectFailed());
    }
    auto err = esp_mqtt_client_register_event(
        mqtt_client_handle_, MQTT_EVENT_ANY,
        [](void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
            ((EspMqtt*)handler_args)->MqttEventCallback(base, event_id, event_data);
        },
        this);
    if (err != ESP_OK) {
        Disconnect();
        return Fail(NetworkError::FromEsp(err));
    }
    err = esp_mqtt_client_start(mqtt_client_handle_);
    if (err != ESP_OK) {
        Disconnect();
        return Fail(NetworkError::FromEsp(err));
    }

    auto bits = xEventGroupWaitBits(
        event_group_handle_, MQTT_CONNECTED_EVENT | MQTT_DISCONNECTED_EVENT | MQTT_ERROR_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
    if ((bits & MQTT_CONNECTED_EVENT) && connected_.load()) {
        return {};
    }
    if (bits & MQTT_ERROR_EVENT) {
        return std::unexpected(last_error_);
    }
    if (bits & (MQTT_CONNECTED_EVENT | MQTT_DISCONNECTED_EVENT)) {
        return Fail(NetworkError::ServerDisconnected());
    }
    return Fail(NetworkError::Timeout());
}

void EspMqtt::MqttEventCallback(esp_event_base_t base, int32_t event_id, void* event_data) {
    auto event = (esp_mqtt_event_t*)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ResetMessageAssembly();
            if (!connected_.exchange(true)) {
                if (on_connected_callback_) {
                    on_connected_callback_();
                }
            }
            xEventGroupSetBits(event_group_handle_, MQTT_CONNECTED_EVENT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ResetMessageAssembly();
            if (connected_.exchange(false)) {
                if (on_disconnected_callback_) {
                    on_disconnected_callback_();
                }
            }
            xEventGroupSetBits(event_group_handle_, MQTT_DISCONNECTED_EVENT);
            break;
        case MQTT_EVENT_DATA: {
            if (!connected_.load() || event == nullptr || event->total_data_len < 0 ||
                static_cast<size_t>(event->total_data_len) > kMaxMessageBytes ||
                event->data_len < 0 || event->current_data_offset < 0 || event->topic_len < 0 ||
                event->topic_len > kMaxTopicBytes ||
                event->current_data_offset > event->total_data_len ||
                event->data_len > event->total_data_len - event->current_data_offset ||
                (event->data_len > 0 && event->data == nullptr) ||
                (event->topic_len > 0 && event->topic == nullptr)) {
                ResetMessageAssembly();
                break;
            }

            const auto total_len = static_cast<size_t>(event->total_data_len);
            const auto offset = static_cast<size_t>(event->current_data_offset);
            const auto data_len = static_cast<size_t>(event->data_len);
            if (offset == 0) {
                ResetMessageAssembly();
                if (event->topic_len == 0) {
                    break;
                }
                std::string topic(event->topic, event->topic_len);
                if (data_len == total_len) {
                    if (on_message_callback_) {
                        on_message_callback_(topic, data_len > 0
                                                        ? std::string(event->data, data_len)
                                                        : std::string());
                    }
                    break;
                }
                message_topic_ = std::move(topic);
                message_total_len_ = total_len;
                message_id_ = event->msg_id;
            } else if (message_total_len_ != total_len || message_payload_.size() != offset ||
                       message_id_ != event->msg_id ||
                       (event->topic_len > 0 &&
                        (message_topic_.size() != static_cast<size_t>(event->topic_len) ||
                         message_topic_.compare(0, message_topic_.size(), event->topic,
                                                event->topic_len) != 0))) {
                ResetMessageAssembly();
                break;
            }

            if (data_len > 0) {
                // Reserve the validated total once, rather than letting
                // std::string grow beyond the receive budget by doubling.
                if (message_payload_.capacity() < message_payload_.size() + data_len) {
                    message_payload_.reserve(total_len);
                }
                message_payload_.append(event->data, data_len);
            }
            if (message_payload_.size() == total_len) {
                auto topic = std::move(message_topic_);
                auto payload = std::move(message_payload_);
                ResetMessageAssembly();
                if (on_message_callback_) {
                    on_message_callback_(topic, payload);
                }
            }
            break;
        }
        case MQTT_EVENT_BEFORE_CONNECT:
            ResetMessageAssembly();
            break;
        case MQTT_EVENT_SUBSCRIBED:
            break;
        case MQTT_EVENT_ERROR: {
            ResetMessageAssembly();
            const esp_err_t tls_error = event != nullptr && event->error_handle != nullptr
                                            ? event->error_handle->esp_tls_last_esp_err
                                            : ESP_FAIL;
            last_error_ = NetworkError::FromEsp(tls_error);
            xEventGroupSetBits(event_group_handle_, MQTT_ERROR_EVENT);
            const char* error_name = esp_err_to_name(tls_error);
            ESP_LOGI(TAG, "MQTT error occurred: %s", error_name);
            if (on_error_callback_) {
                on_error_callback_(error_name ? error_name : "MQTT error");
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "Unhandled event id %ld", event_id);
            break;
    }
}

void EspMqtt::ResetMessageAssembly() {
    std::string().swap(message_payload_);
    std::string().swap(message_topic_);
    message_total_len_ = 0;
    message_id_ = 0;
}

void EspMqtt::Disconnect() {
    if (mqtt_client_handle_ != nullptr) {
        esp_mqtt_client_stop(mqtt_client_handle_);
        esp_mqtt_client_destroy(mqtt_client_handle_);
        mqtt_client_handle_ = nullptr;
    }
    connected_.store(false);
    ResetMessageAssembly();
    xEventGroupClearBits(event_group_handle_,
                         MQTT_CONNECTED_EVENT | MQTT_DISCONNECTED_EVENT | MQTT_ERROR_EVENT);
}

bool EspMqtt::Publish(const std::string topic, const std::string payload, int qos) {
    if (!connected_.load()) {
        return false;
    }
    int msg_id = esp_mqtt_client_publish(mqtt_client_handle_, topic.c_str(), payload.data(),
                                         payload.size(), qos, 0);
    return (qos == 0) ? (msg_id == 0) : (msg_id > 0);
}

bool EspMqtt::Subscribe(const std::string topic, int qos) {
    if (!connected_.load()) {
        return false;
    }
    return esp_mqtt_client_subscribe_single(mqtt_client_handle_, topic.c_str(), qos) > 0;
}

bool EspMqtt::Unsubscribe(const std::string topic) {
    if (!connected_.load()) {
        return false;
    }
    return esp_mqtt_client_unsubscribe(mqtt_client_handle_, topic.c_str()) > 0;
}

bool EspMqtt::IsConnected() { return connected_.load(); }
