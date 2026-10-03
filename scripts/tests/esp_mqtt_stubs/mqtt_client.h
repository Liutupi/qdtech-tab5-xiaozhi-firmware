#pragma once

#include <cstdint>

#include <esp_err.h>

using esp_event_base_t = const char*;

const char* esp_err_to_name(esp_err_t);

struct esp_mqtt_client;
using esp_mqtt_client_handle_t = esp_mqtt_client*;

enum {
    MQTT_EVENT_ANY = -1,
    MQTT_EVENT_ERROR,
    MQTT_EVENT_CONNECTED,
    MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED,
    MQTT_EVENT_DATA,
    MQTT_EVENT_BEFORE_CONNECT,
};

enum {
    MQTT_TRANSPORT_OVER_TCP,
    MQTT_TRANSPORT_OVER_SSL,
};

struct esp_mqtt_error_codes_t {
    esp_err_t esp_tls_last_esp_err = ESP_OK;
};

struct esp_mqtt_event_t {
    esp_mqtt_client_handle_t client = nullptr;
    char* topic = nullptr;
    int topic_len = 0;
    char* data = nullptr;
    int data_len = 0;
    int total_data_len = 0;
    int current_data_offset = 0;
    int msg_id = 0;
    esp_mqtt_error_codes_t* error_handle = nullptr;
};

struct esp_mqtt_client_config_t {
    struct {
        int stack_size = 0;
    } task;
    struct {
        struct {
            const char* hostname = nullptr;
            int port = 0;
            int transport = MQTT_TRANSPORT_OVER_TCP;
        } address;
        struct {
            esp_err_t (*crt_bundle_attach)(void*) = nullptr;
        } verification;
    } broker;
    struct {
        const char* client_id = nullptr;
        const char* username = nullptr;
        struct {
            const char* password = nullptr;
        } authentication;
    } credentials;
    struct {
        int keepalive = 0;
    } session;
};

using esp_mqtt_event_handler_t = void (*)(void*, esp_event_base_t, int32_t, void*);

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t*);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int32_t,
                                         esp_mqtt_event_handler_t, void*);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char*, const char*, int, int, int);
int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t, const char*, int);
int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t, const char*);
