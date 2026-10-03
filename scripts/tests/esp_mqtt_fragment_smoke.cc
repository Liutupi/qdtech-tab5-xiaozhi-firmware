#include <mqtt_client.h>

#include <cassert>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "esp_mqtt.h"

struct esp_mqtt_client {
    esp_mqtt_event_handler_t callback = nullptr;
    void* callback_arg = nullptr;
};

static esp_mqtt_client_handle_t active_client = nullptr;
static bool fail_init = false;
static bool fail_register = false;
static bool fail_start = false;
static bool disconnect_during_start = false;

NetworkError NetworkError::FromEsp(esp_err_t err) { return NetworkError::Unknown(err); }

const char* esp_err_to_name(esp_err_t) { return "fake error"; }

static void Emit(int32_t id, const std::string& topic = {}, const std::string& data = {},
                 int total = 0, int offset = 0, int msg_id = 1) {
    assert(active_client != nullptr);
    esp_mqtt_error_codes_t error{};
    auto event = esp_mqtt_event_t{};
    event.client = active_client;
    event.topic = topic.empty() ? nullptr : const_cast<char*>(topic.data());
    event.topic_len = static_cast<int>(topic.size());
    event.data = data.empty() ? nullptr : const_cast<char*>(data.data());
    event.data_len = static_cast<int>(data.size());
    event.total_data_len = total;
    event.current_data_offset = offset;
    event.msg_id = msg_id;
    event.error_handle = &error;
    active_client->callback(active_client->callback_arg, nullptr, id, &event);
}

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t*) {
    if (fail_init) {
        return nullptr;
    }
    active_client = new esp_mqtt_client;
    return active_client;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int32_t,
                                         esp_mqtt_event_handler_t callback, void* arg) {
    if (fail_register) {
        return ESP_FAIL;
    }
    client->callback = callback;
    client->callback_arg = arg;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t) {
    if (fail_start) {
        return ESP_FAIL;
    }
    Emit(MQTT_EVENT_CONNECTED);
    if (disconnect_during_start) {
        Emit(MQTT_EVENT_DISCONNECTED);
    }
    return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t) {
    if (active_client->callback != nullptr) {
        Emit(MQTT_EVENT_DISCONNECTED);
    }
    return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client) {
    delete client;
    active_client = nullptr;
    return ESP_OK;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char*, const char*, int, int, int) {
    return 0;
}

int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t, const char*, int) { return 1; }

int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t, const char*) { return 1; }

int main() {
    EspMqtt client;
    std::vector<std::pair<std::string, std::string>> messages;
    client.OnMessage([&](const std::string& topic, const std::string& payload) {
        messages.emplace_back(topic, payload);
    });
    assert(client.Connect("broker", 1883, "id", "user", "pass"));
    assert(client.IsConnected());

    // Later ESP-MQTT fragments commonly have no topic. Keep the first one.
    Emit(MQTT_EVENT_DATA, "control/topic", "{\"ty", 16, 0, 7);
    Emit(MQTT_EVENT_DATA, "", "pe\":\"hello\"}", 16, 4, 7);
    assert(messages.size() == 1);
    assert(messages.back().first == "control/topic");
    assert(messages.back().second == "{\"type\":\"hello\"}");

    // A reconnect must discard incomplete bytes; orphaned tails are ignored.
    Emit(MQTT_EVENT_DATA, "control/topic", "old", 6, 0, 8);
    Emit(MQTT_EVENT_DISCONNECTED);
    assert(!client.IsConnected());
    Emit(MQTT_EVENT_CONNECTED);
    Emit(MQTT_EVENT_DATA, "", "end", 6, 3, 8);
    assert(messages.size() == 1);
    Emit(MQTT_EVENT_DATA, "control/topic", "new", 3, 0, 9);
    assert(messages.size() == 2 && messages.back().second == "new");

    // Wrong offsets and message identities cannot complete a prior payload.
    Emit(MQTT_EVENT_DATA, "control/topic", "abc", 6, 0, 10);
    Emit(MQTT_EVENT_DATA, "", "def", 6, 2, 10);
    Emit(MQTT_EVENT_DATA, "", "def", 6, 3, 10);
    assert(messages.size() == 2);
    Emit(MQTT_EVENT_DATA, "control/topic", "abc", 6, 0, 11);
    Emit(MQTT_EVENT_DATA, "", "def", 6, 3, 12);
    assert(messages.size() == 2);

    // Oversize totals are rejected before allocation and do not poison next message.
#if defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_USE_MALLOC
    constexpr int kMessageLimit = 128 * 1024;
#else
    constexpr int kMessageLimit = 16 * 1024;
#endif
    Emit(MQTT_EVENT_DATA, "control/topic", "x", kMessageLimit + 1, 0, 13);
    Emit(MQTT_EVENT_DATA, "control/topic", "ok", 2, 0, 14);
    assert(messages.size() == 3 && messages.back().second == "ok");

    // The exact cap still permits one complete fragmented control message.
    const std::string first_half(kMessageLimit / 2, 'a');
    const std::string second_half(kMessageLimit - first_half.size(), 'b');
    Emit(MQTT_EVENT_DATA, "control/topic", first_half, kMessageLimit, 0, 17);
    Emit(MQTT_EVENT_DATA, "", second_half, kMessageLimit, first_half.size(), 17);
    assert(messages.size() == 4);
    assert(messages.back().second.size() == kMessageLimit);
    assert(messages.back().second.front() == 'a');
    assert(messages.back().second.back() == 'b');

    // MQTT errors discard the partial frame even if the connection remains up.
    Emit(MQTT_EVENT_DATA, "control/topic", "abc", 6, 0, 15);
    Emit(MQTT_EVENT_ERROR);
    Emit(MQTT_EVENT_DATA, "", "def", 6, 3, 15);
    assert(messages.size() == 4);

    client.Disconnect();
    assert(!client.IsConnected());
    assert(client.Connect("broker", 1883, "id", "user", "pass"));
    Emit(MQTT_EVENT_DATA, "control/topic", "ready", 5, 0, 16);
    assert(messages.size() == 5 && messages.back().second == "ready");

    // A successful CONNECTED event followed immediately by disconnect cannot
    // make Connect report a live connection.
    client.Disconnect();
    disconnect_during_start = true;
    assert(!client.Connect("broker", 1883, "id", "user", "pass"));
    assert(!client.IsConnected());
    disconnect_during_start = false;

    // Setup failures cleanly release the client and allow a later retry.
    fail_init = true;
    assert(!client.Connect("broker", 1883, "id", "user", "pass"));
    fail_init = false;
    fail_register = true;
    assert(!client.Connect("broker", 1883, "id", "user", "pass"));
    assert(active_client == nullptr);
    fail_register = false;
    fail_start = true;
    assert(!client.Connect("broker", 1883, "id", "user", "pass"));
    assert(active_client == nullptr);
    fail_start = false;
    assert(client.Connect("broker", 1883, "id", "user", "pass"));
    assert(client.IsConnected());

    std::cout << "MQTT fragments and reconnect lifecycle passed\n";
}
