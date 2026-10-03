#ifndef _ESP_TCP_H_
#define _ESP_TCP_H_

#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>

class EspTcp : public Tcp {
public:
    EspTcp();
    ~EspTcp();

    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    void Interrupt() override;
    int Send(const std::string& data) override;

private:
    int tcp_fd_ = -1;
    std::mutex socket_mutex_;
    std::atomic_bool interrupted_{false};
    TaskHandle_t receive_task_handle_ = nullptr;
    // The task sets this after its final access to EspTcp. It may then call
    // vTaskDelete(NULL), which does not access this object.
    std::atomic_bool receive_task_finished_{true};
    std::mutex send_mutex_;

    void ReceiveTask();
};

#endif  // _ESP_TCP_H_
