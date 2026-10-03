#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "network_error.h"
#include "tcp.h"

class NetworkInterface;

class WebSocket {
public:
    WebSocket(NetworkInterface* network, int connect_id);
    ~WebSocket();

    void SetHeader(const char* key, const char* value);
    void SetReceiveBufferSize(size_t size);
    bool IsConnected() const;
    NetworkResult<> Connect(const char* uri);
    bool Send(const std::string& data);
    bool Send(const void* data, size_t len, bool binary = false, bool fin = true);
    void Ping();
    void Close();
    // Safe to call from another task during Connect or Send. Only signals
    // cancellation; Abort()/destruction remain on the owning task.
    void Interrupt();
    // Interrupt an in-flight Send before the owner waits for senders to finish.
    // The owner must serialize Connect/Abort; Send may run on another task.
    void Abort();

    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnData(std::function<void(const char*, size_t, bool binary)> callback);
    void OnError(std::function<void(const NetworkError& error)> callback);
    // Invoked when a Pong control frame (opcode 0xA) is received from the
    // peer, carrying the Pong's application data payload (which echoes the
    // payload of the Ping that triggered it). Lets callers implement an
    // active liveness check (send Ping(), expect OnPong within a timeout)
    // or measure round-trip time, without the library imposing any timeout
    // policy of its own.
    void OnPong(std::function<void(const char*, size_t)> callback);

private:
    NetworkResult<> Fail(NetworkError err) {
        last_error_ = err;
        return std::unexpected(err);
    }

    NetworkError last_error_{};
    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;
    // Connect replaces tcp_; Interrupt must not observe a freed transport.
    std::mutex tcp_mutex_;
    std::atomic_bool interrupted_{false};
    bool continuation_ = false;
    size_t receive_buffer_size_ = 2048;
    std::string receive_buffer_;
    bool handshake_completed_ = false;
    // Serialize connection state transitions and their public callbacks.
    std::mutex callback_mutex_;
    mutable std::mutex state_mutex_;
    bool handshake_pending_ = false;
    bool handshake_failed_ = false;
    bool connected_ = false;

    // Mutex for sending data and replying pong
    std::mutex send_mutex_;

    EventGroupHandle_t handshake_event_group_;
    static const EventBits_t HANDSHAKE_SUCCESS_BIT = BIT0;
    static const EventBits_t HANDSHAKE_FAILED_BIT = BIT1;

    std::map<std::string, std::string> headers_;
    std::function<void(const char*, size_t, bool binary)> on_data_;
    std::function<void(const NetworkError& error)> on_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(const char*, size_t)> on_pong_;

    std::vector<char> current_message_;
    bool is_fragmented_ = false;
    bool is_binary_ = false;

    void OnTcpData(const std::string& data);
    void HandleTcpDisconnected();
    void RejectIncomingData(const char* reason);
    bool SendControlFrame(uint8_t opcode, const void* data, size_t len);
};

#endif  // WEBSOCKET_H
