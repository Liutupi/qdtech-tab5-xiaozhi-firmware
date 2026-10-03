#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <web_socket.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include "protocol.h"

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define WEBSOCKET_PROTOCOL_CANCEL_EVENT (1 << 1)
#define WEBSOCKET_PROTOCOL_DISCONNECTED_EVENT (1 << 2)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CancelOpen() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    struct ConnectionAttempt {
        explicit ConnectionAttempt(uint64_t generation) : generation(generation) {
            event_group = xEventGroupCreate();
        }
        ~ConnectionAttempt() {
            if (event_group != nullptr) {
                vEventGroupDelete(event_group);
            }
        }

        const uint64_t generation;
        EventGroupHandle_t event_group = nullptr;
        int version = 1;
        bool hello_received = false;
        bool disconnected = false;
    };

    mutable std::mutex channel_mutex_;
    std::condition_variable send_done_;
    std::mutex send_mutex_;
    std::shared_ptr<WebSocket> websocket_;
    std::shared_ptr<ConnectionAttempt> attempt_;
    uint64_t generation_ = 0;
    size_t active_sends_ = 0;
    bool opening_ = false;
    bool opened_ = false;
    bool cancel_next_open_ = false;
    int version_ = 1;

    bool IsCurrentLocked(const std::shared_ptr<ConnectionAttempt>& attempt) const;
    void HandleData(const std::shared_ptr<ConnectionAttempt>& attempt, const char* data, size_t len,
                    bool binary);
    void HandleDisconnected(const std::shared_ptr<ConnectionAttempt>& attempt);
    void ParseServerHello(const std::shared_ptr<ConnectionAttempt>& attempt, const cJSON* root);
    void ReportError(const std::shared_ptr<ConnectionAttempt>& attempt, const std::string& message);
    void FinishSend();
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage(int version);
};

#endif
