#ifndef _ESP_SSL_H_
#define _ESP_SSL_H_

#include <esp_tls.h>
#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>

class EspSsl : public Tcp {
public:
    EspSsl();
    ~EspSsl();

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    void Interrupt() override;
    int Send(const std::string& data) override;

private:
    esp_tls_t* tls_client_ = nullptr;
    std::atomic_int socket_fd_{-1};
    std::mutex socket_mutex_;
    std::atomic_bool interrupted_{false};
    TaskHandle_t receive_task_handle_ = nullptr;
    std::atomic_bool receive_task_finished_{true};
    std::mutex send_mutex_;

    void ReceiveTask();
};

#endif  // _ESP_SSL_H_
