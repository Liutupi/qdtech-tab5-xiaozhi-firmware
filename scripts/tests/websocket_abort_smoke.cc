// Build the real WebSocket with a TCP double whose data send blocks until
// Disconnect. Abort must release the sender without taking its send lock.
#include "network_interface.h"
#include "web_socket.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

NetworkError NetworkError::FromErrno(int error) { return ConnectFailed(error); }
NetworkError NetworkError::FromHErrno(int error) { return DnsFailed(error); }
NetworkError NetworkError::FromEsp(esp_err_t error) { return TlsFailed(error); }
std::string NetworkError::ToString() const { return "test error"; }

static void Check(bool okay, const char* message) {
    if (!okay) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

template <typename Predicate>
static bool WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct TcpState {
    std::mutex mutex;
    std::condition_variable cv;
    bool sender_entered = false;
    bool connect_entered = false;
    bool block_connect = false;
    bool interrupted = false;
    bool disconnected = false;
};

class BlockingTcp : public Tcp {
public:
    explicit BlockingTcp(std::shared_ptr<TcpState> state) : state_(std::move(state)) {}

    NetworkResult<> Connect(const std::string&, int) override {
        if (state_->block_connect) {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->connect_entered = true;
            state_->cv.notify_all();
            state_->cv.wait(lock, [&]() { return state_->interrupted; });
            return std::unexpected(NetworkError::ServerDisconnected());
        }
        connected_ = true;
        return {};
    }

    void Interrupt() override {
        connected_ = false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->interrupted = true;
        state_->cv.notify_all();
    }

    void Disconnect() override {
        Interrupt();
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->disconnected = true;
            state_->cv.notify_all();
        }
        if (disconnect_callback_)
            disconnect_callback_();
    }

    int Send(const std::string& data) override {
        if (data.starts_with("GET ")) {
            // The handshake is delivered before Connect begins its event wait.
            stream_callback_("HTTP/1.1 101 Switching Protocols\r\n\r\n");
            return static_cast<int>(data.size());
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->sender_entered = true;
        state_->cv.notify_all();
        state_->cv.wait(lock, [&]() { return state_->interrupted; });
        return -1;
    }

private:
    std::shared_ptr<TcpState> state_;
};

class TestNetwork : public NetworkInterface {
public:
    explicit TestNetwork(std::shared_ptr<TcpState> state) : state_(std::move(state)) {}
    std::unique_ptr<Http> CreateHttp(int) override { return nullptr; }
    std::unique_ptr<Tcp> CreateTcp(int) override { return std::make_unique<BlockingTcp>(state_); }
    std::unique_ptr<Tcp> CreateSsl(int) override { return nullptr; }
    std::unique_ptr<Udp> CreateUdp(int) override { return nullptr; }
    std::unique_ptr<Mqtt> CreateMqtt(int) override { return nullptr; }
    std::unique_ptr<WebSocket> CreateWebSocket(int) override { return nullptr; }

private:
    std::shared_ptr<TcpState> state_;
};

int main() {
    for (const bool physical_abort : {false, true}) {
        auto state = std::make_shared<TcpState>();
        TestNetwork network(state);
        WebSocket websocket(&network, 1);
        Check(websocket.Connect("ws://127.0.0.1/test").has_value(), "handshake failed");

        std::atomic_bool sender_finished{false};
        std::atomic_bool cancel_finished{false};
        std::thread sender([&]() {
            Check(!websocket.Send("data"), "canceled send unexpectedly succeeded");
            sender_finished = true;
        });
        Check(WaitUntil([&]() {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  return state->sender_entered;
              }),
              "data send never entered TCP");
        std::thread canceler([&]() {
            if (physical_abort) {
                websocket.Abort();
            } else {
                websocket.Interrupt();
            }
            cancel_finished = true;
        });
        Check(WaitUntil([&]() { return cancel_finished.load() && sender_finished.load(); }),
              "cancel did not release blocked send");
        canceler.join();
        sender.join();
        if (!physical_abort) {
            std::lock_guard<std::mutex> lock(state->mutex);
            Check(!state->disconnected, "Interrupt performed a blocking Disconnect");
        }
        Check(!websocket.Send("later"), "interrupted socket accepted another send");
    }
    {
        auto state = std::make_shared<TcpState>();
        state->block_connect = true;
        TestNetwork network(state);
        WebSocket websocket(&network, 1);
        std::atomic_bool open_finished{false};
        std::thread opener([&]() {
            Check(!websocket.Connect("ws://127.0.0.1/test").has_value(),
                  "interrupted connection unexpectedly succeeded");
            open_finished = true;
        });
        Check(WaitUntil([&]() {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  return state->connect_entered;
              }),
              "connection never reached TCP");
        websocket.Interrupt();
        Check(WaitUntil([&]() { return open_finished.load(); }),
              "Interrupt did not release a pending Connect");
        opener.join();
    }
    std::cout << "WebSocket Interrupt and Abort released blocked operations" << std::endl;
}
