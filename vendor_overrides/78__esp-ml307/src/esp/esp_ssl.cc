#include "esp_ssl.h"
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

static const char* TAG = "EspSsl";
static constexpr int kConnectDeadlineMs = 10000;
static constexpr int kSendTimeoutMs = 3000;
static constexpr int kSendCallDeadlineMs = 5000;

EspSsl::EspSsl() = default;

EspSsl::~EspSsl() { Disconnect(); }

NetworkResult<> EspSsl::Connect(const std::string& host, int port) {
    if (interrupted_.load(std::memory_order_acquire)) {
        return Fail(NetworkError::ServerDisconnected());
    }
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return Fail(NetworkError::ProtocolError());
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return Fail(NetworkError::TlsFailed());
    }

    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // Connect runs synchronously before esp-tls publishes its socket, so
    // Interrupt cannot stop an in-progress handshake. Bound that wait.
    cfg.timeout_ms = kConnectDeadlineMs;

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        NetworkError err = NetworkError::TlsFailed();
        esp_tls_error_handle_t tls_error;
        if (esp_tls_get_error_handle(tls_client_, &tls_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t esp_err =
                esp_tls_get_and_clear_last_error(tls_error, &error_code, &error_flags);
            err = NetworkError::FromEsp(esp_err);
            ESP_LOGE(TAG, "Failed to connect to %s:%d: %s", host.c_str(), port,
                     err.ToString().c_str());
        } else {
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return Fail(err);
    }

    int connected_fd = -1;
    if (esp_tls_get_conn_sockfd(tls_client_, &connected_fd) != ESP_OK || connected_fd < 0) {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return Fail(NetworkError::TlsFailed());
    }
    const timeval send_timeout = {kSendTimeoutMs / 1000, (kSendTimeoutMs % 1000) * 1000};
    if (setsockopt(connected_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) !=
        0) {
        const auto err = NetworkError::FromErrno(errno);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return Fail(err);
    }
    bool canceled;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        canceled = interrupted_.load(std::memory_order_acquire);
        if (!canceled) {
            socket_fd_.store(connected_fd, std::memory_order_release);
            connected_ = true;
        }
    }
    if (canceled) {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return Fail(NetworkError::ServerDisconnected());
    }

    receive_task_finished_.store(false, std::memory_order_relaxed);
    auto created = xTaskCreate(
        [](void* arg) {
            EspSsl* ssl = (EspSsl*)arg;
            ssl->ReceiveTask();
            // Final access to ssl before the task deletes itself.
            ssl->receive_task_finished_.store(true, std::memory_order_release);
            vTaskDelete(NULL);
        },
        "ssl_receive", 4096, this, 1, &receive_task_handle_);
    if (created != pdPASS) {
        connected_ = false;
        receive_task_finished_.store(true, std::memory_order_release);
        receive_task_handle_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            socket_fd_.store(-1, std::memory_order_release);
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        ESP_LOGE(TAG, "Failed to start TLS receive task");
        return Fail(NetworkError::NotInitialized());
    }
    return {};
}

void EspSsl::Disconnect() {
    connected_ = false;

    // Do not touch tls_client_ while Send may be inside esp_tls_conn_write().
    // Keep this descriptor owned by esp_tls_conn_destroy() until both tasks stop.
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        const int sockfd = socket_fd_.load(std::memory_order_relaxed);
        if (sockfd >= 0) {
            shutdown(sockfd, SHUT_RDWR);
        }
    }

    if (receive_task_handle_ != nullptr) {
        if (!receive_task_finished_.load(std::memory_order_acquire) &&
            receive_task_handle_ == xTaskGetCurrentTaskHandle()) {
            // The owner must release this EspSsl after its receive callback returns.
            return;
        }
        unsigned waited_ticks = 0;
        while (!receive_task_finished_.load(std::memory_order_acquire)) {
            vTaskDelay(1);
            if (++waited_ticks % (configTICK_RATE_HZ * 10) == 0) {
                ESP_LOGE(TAG, "Still waiting for TLS receive callback to finish");
            }
        }
        receive_task_handle_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(send_mutex_);
    {
        std::lock_guard<std::mutex> socket_lock(socket_mutex_);
        socket_fd_.store(-1, std::memory_order_release);
    }
    if (tls_client_ != nullptr) {
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

void EspSsl::Interrupt() {
    interrupted_.store(true, std::memory_order_release);
    connected_ = false;
    std::lock_guard<std::mutex> lock(socket_mutex_);
    const int sockfd = socket_fd_.load(std::memory_order_relaxed);
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
    }
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!connected_ || tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
    const TickType_t started = xTaskGetTickCount();

    while (total_sent < data_size) {
        if (!connected_ || interrupted_.load(std::memory_order_acquire)) {
            return -1;
        }
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(kSendCallDeadlineMs)) {
            ESP_LOGE(TAG, "SSL send deadline exceeded after %zu/%zu bytes", total_sent, data_size);
            return -1;
        }
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
            continue;
        }

        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());

        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }

        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            }
            connected_ = false;
            // 接收失败或连接断开时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}
