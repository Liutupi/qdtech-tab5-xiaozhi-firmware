#include "esp_tcp.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

static const char* TAG = "EspTcp";
static constexpr int kSendTimeoutMs = 3000;
static constexpr int kSendCallDeadlineMs = 5000;
static constexpr uint32_t kSendPollMs = 100;

EspTcp::EspTcp() = default;

EspTcp::~EspTcp() { Disconnect(); }

NetworkResult<> EspTcp::Connect(const std::string& host, int port) {
    if (interrupted_.load(std::memory_order_acquire)) {
        return Fail(NetworkError::ServerDisconnected());
    }
    if (receive_task_handle_ != nullptr &&
        !receive_task_finished_.load(std::memory_order_acquire) &&
        receive_task_handle_ == xTaskGetCurrentTaskHandle()) {
        // A stream callback cannot restart its own receive task in place.
        return Fail(NetworkError::ProtocolError());
    }
    // A passive disconnect may already have cleared connected_ while its
    // receive task is still running. Join that task before reusing this object.
    if (receive_task_handle_ != nullptr || tcp_fd_ != -1) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent* server = gethostbyname(host.c_str());
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to get host by name");
        return Fail(NetworkError::FromHErrno(h_errno));
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);
    ESP_LOGI(TAG, "Resolved %s -> %s", host.c_str(), inet_ntoa(*(struct in_addr*)server->h_addr));

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return Fail(NetworkError::FromErrno(errno));
    }
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        tcp_fd_ = fd;
        if (interrupted_.load(std::memory_order_acquire)) {
            shutdown(fd, SHUT_RDWR);
        }
    }

    // A peer that stops reading must not keep the protocol send worker (and
    // consequently channel teardown) waiting on one send indefinitely.
    const timeval send_timeout = {kSendTimeoutMs / 1000, (kSendTimeoutMs % 1000) * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) {
        const auto err = NetworkError::FromErrno(errno);
        ESP_LOGE(TAG, "Failed to set TCP send timeout: %s", err.ToString().c_str());
        std::lock_guard<std::mutex> lock(socket_mutex_);
        close(fd);
        tcp_fd_ = -1;
        return Fail(err);
    }

    if (interrupted_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        close(fd);
        tcp_fd_ = -1;
        return Fail(NetworkError::ServerDisconnected());
    }
    int ret = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        auto err = NetworkError::FromErrno(errno);
        ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port, err.ToString().c_str());
        std::lock_guard<std::mutex> lock(socket_mutex_);
        close(fd);
        tcp_fd_ = -1;
        return Fail(err);
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (interrupted_.load(std::memory_order_acquire)) {
            close(fd);
            tcp_fd_ = -1;
            return Fail(NetworkError::ServerDisconnected());
        }
        connected_ = true;
    }

    receive_task_finished_.store(false, std::memory_order_relaxed);
    auto created = xTaskCreate(
        [](void* arg) {
            EspTcp* tcp = (EspTcp*)arg;
            tcp->ReceiveTask();
            // This release store is the final use of tcp. The owner may free
            // it after observing true with an acquire load below.
            tcp->receive_task_finished_.store(true, std::memory_order_release);
            vTaskDelete(NULL);
        },
        "tcp_receive", 4096, this, 1, &receive_task_handle_);
    if (created != pdPASS) {
        connected_ = false;
        receive_task_finished_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(socket_mutex_);
        close(fd);
        tcp_fd_ = -1;
        receive_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to start TCP receive task");
        return Fail(NetworkError::NotInitialized());
    }
    return {};
}

void EspTcp::Disconnect() {
    connected_ = false;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (tcp_fd_ != -1) {
            // Wake recv/send without releasing the descriptor number.
            shutdown(tcp_fd_, SHUT_RDWR);
        }
    }

    if (receive_task_handle_ != nullptr) {
        if (!receive_task_finished_.load(std::memory_order_acquire) &&
            receive_task_handle_ == xTaskGetCurrentTaskHandle()) {
            // Disconnect from a stream/disconnect callback is allowed. The
            // owner must release this EspTcp after that callback returns.
            return;
        }
        unsigned waited_ticks = 0;
        while (!receive_task_finished_.load(std::memory_order_acquire)) {
            vTaskDelay(1);
            if (++waited_ticks % (configTICK_RATE_HZ * 10) == 0) {
                ESP_LOGE(TAG, "Still waiting for TCP receive callback to finish");
            }
        }
        receive_task_handle_ = nullptr;
    }

    // Send may be in flight on another task. Wait for it before releasing the
    // descriptor, so lwIP cannot reuse its number under that sender.
    std::lock_guard<std::mutex> lock(send_mutex_);
    std::lock_guard<std::mutex> socket_lock(socket_mutex_);
    if (tcp_fd_ != -1) {
        close(tcp_fd_);
        tcp_fd_ = -1;
    }
}

void EspTcp::Interrupt() {
    interrupted_.store(true, std::memory_order_release);
    connected_ = false;
    // Never wait for send_mutex_ or the receive task here. Holding the short
    // descriptor lock through shutdown prevents a concurrent close/reuse race.
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (tcp_fd_ != -1) {
        shutdown(tcp_fd_, SHUT_RDWR);
    }
}

int EspTcp::Send(const std::string& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!connected_ || interrupted_.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    const TickType_t started = xTaskGetTickCount();

    while (total_sent < data_size) {
        if (interrupted_.load(std::memory_order_acquire)) {
            return -1;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= pdMS_TO_TICKS(kSendCallDeadlineMs)) {
            ESP_LOGE(TAG, "TCP send deadline exceeded after %zu/%zu bytes", total_sent, data_size);
            return -1;
        }
        // MSG_DONTWAIT lets select enforce a deadline for the whole message.
        // SO_SNDTIMEO remains a fallback if a platform ignores that flag.
        const size_t chunk_size = std::min<size_t>(4096, data_size - total_sent);
        int ret = send(tcp_fd_, data_ptr + total_sent, chunk_size, MSG_DONTWAIT);
        if (ret > 0) {
            total_sent += ret;
            continue;
        }
        if (ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return -1;
        }
        if (errno == EINTR)
            continue;

        const TickType_t spent = xTaskGetTickCount() - started;
        if (spent >= pdMS_TO_TICKS(kSendCallDeadlineMs)) {
            ESP_LOGE(TAG, "TCP send deadline exceeded after %zu/%zu bytes", total_sent, data_size);
            return -1;
        }
        const TickType_t remaining = pdMS_TO_TICKS(kSendCallDeadlineMs) - spent;
        // Some lwIP versions do not wake select immediately on shutdown.
        // Poll briefly so Interrupt always becomes visible to this sender.
        const uint32_t wait_ms = std::min<uint32_t>(remaining * portTICK_PERIOD_MS, kSendPollMs);
        timeval wait = {static_cast<time_t>(wait_ms / 1000),
                        static_cast<suseconds_t>((wait_ms % 1000) * 1000)};
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(tcp_fd_, &writable);
        ret = select(tcp_fd_ + 1, nullptr, &writable, nullptr, &wait);
        if (ret < 0 && errno == EINTR)
            continue;
        if (ret == 0) {
            continue;
        }
        if (ret < 0) {
            ESP_LOGE(TAG, "TCP send wait failed: ret=%d, errno=%d", ret, errno);
            return -1;
        }
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
    connected_ = false;
    // Keep tcp_fd_ alive until the owner observes our final object access.
    // On passive disconnect the callback may release its WebSocket owner.
    if (disconnect_callback_) {
        disconnect_callback_();
    }
}
