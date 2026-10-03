// Build the real local EspTcp source with a small FreeRTOS shim. Closing a peer
// while OnDisconnected is still running must keep the owner destructor waiting.
#include "esp_ssl.h"
#include "esp_tcp.h"
#include "http_client.h"
#include "network_interface.h"
#include "web_socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
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

class TestNetwork : public NetworkInterface {
public:
    std::unique_ptr<Http> CreateHttp(int) override { return nullptr; }
    std::unique_ptr<Tcp> CreateTcp(int) override { return std::make_unique<EspTcp>(); }
    std::unique_ptr<Tcp> CreateSsl(int) override { return std::make_unique<EspSsl>(); }
    std::unique_ptr<Udp> CreateUdp(int) override { return nullptr; }
    std::unique_ptr<Mqtt> CreateMqtt(int) override { return nullptr; }
    std::unique_ptr<WebSocket> CreateWebSocket(int) override { return nullptr; }
};

struct ControlledTcpState {
    std::mutex mutex;
    std::condition_variable cv;
    bool passive_close = false;
    bool disconnect_callback_returned = false;
    bool disconnect_called = false;
    bool allow_exit = false;
};

class ControlledTcp : public Tcp {
public:
    explicit ControlledTcp(std::shared_ptr<ControlledTcpState> state) : state_(std::move(state)) {}
    ~ControlledTcp() override {
        if (worker_.joinable())
            worker_.join();
    }
    NetworkResult<> Connect(const std::string&, int) override {
        connected_ = true;
        worker_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->cv.wait(lock, [&]() { return state_->passive_close; });
            connected_ = false;
            lock.unlock();
            if (disconnect_callback_)
                disconnect_callback_();
            lock.lock();
            state_->disconnect_callback_returned = true;
            state_->cv.notify_all();
            state_->cv.wait(lock, [&]() { return state_->allow_exit; });
        });
        return {};
    }
    void Disconnect() override {
        connected_ = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->disconnect_called = true;
            state_->cv.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }
    int Send(const std::string& data) override { return static_cast<int>(data.size()); }

private:
    std::shared_ptr<ControlledTcpState> state_;
    std::thread worker_;
};

class ControlledNetwork : public TestNetwork {
public:
    explicit ControlledNetwork(std::shared_ptr<ControlledTcpState> state)
        : state_(std::move(state)) {}
    std::unique_ptr<Tcp> CreateTcp(int) override { return std::make_unique<ControlledTcp>(state_); }

private:
    std::shared_ptr<ControlledTcpState> state_;
};

static int StartListener(uint16_t& port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    Check(listener >= 0, "socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    Check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "bind failed");
    Check(listen(listener, 1) == 0, "listen failed");
    socklen_t address_length = sizeof(address);
    Check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0,
          "getsockname failed");
    port = ntohs(address.sin_port);
    return listener;
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    for (int round = 0; round < 24; ++round) {
        uint16_t port;
        const int listener = StartListener(port);

        std::mutex mutex;
        std::condition_variable cv;
        bool server_may_close = false;
        bool callback_entered = false;
        bool callback_may_return = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "accept failed");
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() { return server_may_close; });
            }
            close(peer);
            close(listener);
        });

        auto tcp = std::make_unique<EspTcp>();
        Check(tcp->Connect("127.0.0.1", port).has_value(), "connect failed");
        tcp->OnDisconnected([&]() {
            std::unique_lock<std::mutex> lock(mutex);
            callback_entered = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return callback_may_return; });
        });
        {
            std::lock_guard<std::mutex> lock(mutex);
            server_may_close = true;
            cv.notify_all();
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return callback_entered; }),
                  "disconnect callback did not start");
        }

        const int waits_before = fake_delay_calls.load();
        std::atomic_bool destroyed{false};
        std::thread owner([tcp = std::move(tcp), &destroyed]() mutable {
            tcp.reset();
            destroyed = true;
        });
        Check(WaitUntil([&]() { return fake_delay_calls.load() > waits_before; }),
              "destructor did not wait for receive task");
        Check(!destroyed.load(), "destructor freed a live receive task");
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback_may_return = true;
            cv.notify_all();
        }
        owner.join();
        server.join();
        Check(destroyed.load(), "destructor did not finish after callback");
    }

    // A peer that accepts the connection but never reads must release the
    // protocol send worker within the configured socket timeout.
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::mutex mutex;
        std::condition_variable cv;
        bool peer_ready = false;
        bool allow_close = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "stalled-send accept failed");
            const int receive_buffer = 4096;
            Check(setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                             sizeof(receive_buffer)) == 0,
                  "stalled-send receive buffer setup failed");
            {
                std::unique_lock<std::mutex> lock(mutex);
                peer_ready = true;
                cv.notify_all();
                cv.wait(lock, [&]() { return allow_close; });
            }
            close(peer);
            close(listener);
        });
        EspTcp tcp;
        Check(tcp.Connect("127.0.0.1", port).has_value(), "stalled-send connect failed");
        {
            std::unique_lock<std::mutex> lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return peer_ready; }),
                  "stalled-send peer did not start");
        }
        const std::string payload(8 * 1024 * 1024, 'x');
        const auto started = std::chrono::steady_clock::now();
        const int sent = tcp.Send(payload);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        Check(sent == -1, "stalled TCP send unexpectedly succeeded");
        Check(elapsed < std::chrono::seconds(12), "stalled TCP send exceeded timeout");
        {
            std::lock_guard<std::mutex> lock(mutex);
            allow_close = true;
            cv.notify_all();
        }
        tcp.Disconnect();
        server.join();
    }

    TestNetwork network;
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::thread server([=]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "early-close accept failed");
            close(peer);
            close(listener);
        });
        WebSocket websocket(&network, 1);
        const auto url = "ws://127.0.0.1:" + std::to_string(port) + "/";
        const auto started = std::chrono::steady_clock::now();
        Check(!websocket.Connect(url.c_str()).has_value(), "early-close handshake succeeded");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        Check(elapsed < std::chrono::seconds(2), "early close waited for handshake timeout");
        server.join();
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::mutex mutex;
        std::condition_variable cv;
        bool callback_entered = false;
        bool callback_may_return = false;
        bool server_may_close = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "websocket accept failed");
            char request[512];
            Check(recv(peer, request, sizeof(request), 0) > 0, "handshake request missing");
            const char response[] = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
            Check(send(peer, response, sizeof(response) - 1, 0) > 0, "handshake reply failed");
            const char frame[] = {'\x81', '\x01', 'x'};
            Check(send(peer, frame, sizeof(frame), 0) > 0, "websocket frame failed");
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() { return server_may_close; });
            }
            close(peer);
            close(listener);
        });
        auto websocket = std::make_unique<WebSocket>(&network, 1);
        websocket->OnData([&](const char*, size_t, bool) {
            std::unique_lock<std::mutex> lock(mutex);
            callback_entered = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return callback_may_return; });
        });
        const auto url = "ws://127.0.0.1:" + std::to_string(port) + "/";
        Check(websocket->Connect(url.c_str()).has_value(), "websocket handshake failed");
        {
            std::unique_lock<std::mutex> lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return callback_entered; }),
                  "websocket callback did not start");
        }
        const int waits_before = fake_delay_calls.load();
        std::atomic_bool destroyed{false};
        std::thread owner([websocket = std::move(websocket), &destroyed]() mutable {
            websocket.reset();
            destroyed = true;
        });
        Check(WaitUntil([&]() { return fake_delay_calls.load() > waits_before; }),
              "websocket destructor did not wait for receive callback");
        Check(!destroyed.load(), "websocket event group freed during callback");
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback_may_return = true;
            server_may_close = true;
            cv.notify_all();
        }
        owner.join();
        server.join();
    }
    {
        auto state = std::make_shared<ControlledTcpState>();
        ControlledNetwork controlled_network(state);
        auto http = std::make_unique<HttpClient>(&controlled_network);
        Check(http->Open("GET", "http://127.0.0.1:8000/test").has_value(),
              "controlled HTTP open failed");
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->passive_close = true;
            state->cv.notify_all();
            Check(state->cv.wait_for(lock, std::chrono::seconds(2),
                                     [&]() { return state->disconnect_callback_returned; }),
                  "controlled HTTP passive callback did not finish");
        }
        const int deleted_before = fake_deleted_groups.load();
        std::atomic_bool destroyed{false};
        std::thread owner([http = std::move(http), &destroyed]() mutable {
            http.reset();
            destroyed = true;
        });
        Check(WaitUntil([&]() {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  return state->disconnect_called;
              }),
              "HTTP destructor skipped TCP join after passive disconnect");
        Check(!destroyed.load(), "HTTP owner freed while TCP callback task was live");
        Check(fake_deleted_groups.load() == deleted_before,
              "HTTP event group freed before TCP task joined");
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->allow_exit = true;
            state->cv.notify_all();
        }
        owner.join();
        Check(destroyed.load(), "HTTP destructor did not finish after TCP task");
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::thread server([=]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "empty-body HTTP accept failed");
            auto read_request = [peer]() {
                std::string request;
                char buffer[512];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const int count = recv(peer, buffer, sizeof(buffer), 0);
                    Check(count > 0, "keep-alive HTTP request missing");
                    request.append(buffer, static_cast<size_t>(count));
                }
                return request;
            };
            Check(read_request().find("GET /empty HTTP/1.1") == 0,
                  "first keep-alive HTTP request incorrect");
            const std::string empty_response =
                "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
            Check(send(peer, empty_response.data(), empty_response.size(), 0) ==
                      static_cast<ssize_t>(empty_response.size()),
                  "empty-body HTTP response failed");
            Check(read_request().find("GET /next HTTP/1.1") == 0,
                  "HTTP connection was not reused after empty body");
            const std::string next_response =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            Check(send(peer, next_response.data(), next_response.size(), 0) ==
                      static_cast<ssize_t>(next_response.size()),
                  "second keep-alive HTTP response failed");
            close(peer);
            close(listener);
        });
        HttpClient http(&network);
        http.SetTimeout(1000);
        http.SetKeepAlive(true);
        const std::string base_url = "http://127.0.0.1:" + std::to_string(port);
        Check(http.Open("GET", base_url + "/empty").has_value(), "empty-body HTTP open failed");
        Check(http.GetStatusCode().has_value(), "empty-body HTTP status missing");
        char body[8];
        const auto empty_read = http.Read(body, sizeof(body));
        Check(empty_read.has_value() && *empty_read == 0,
              "Content-Length: 0 waited for keep-alive timeout");
        Check(http.ReadAll().empty(), "zero-length HTTP ReadAll should be empty");
        Check(http.IsConnectionReusable("127.0.0.1", port),
              "empty-body HTTP connection should be reusable");
        Check(http.Open("GET", base_url + "/next").has_value(),
              "second keep-alive HTTP open failed");
        Check(http.GetStatusCode().has_value(), "second HTTP status missing");
        Check(http.ReadAll() == "ok", "second HTTP response body incorrect");
        server.join();
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::mutex mutex;
        std::condition_variable cv;
        bool server_may_close = false;
        bool callback_entered = false;
        bool callback_may_return = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "TLS accept failed");
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() { return server_may_close; });
            }
            close(peer);
            close(listener);
        });
        auto tls = std::make_unique<EspSsl>();
        Check(tls->Connect("127.0.0.1", port).has_value(), "TLS connect failed");
        tls->OnDisconnected([&]() {
            std::unique_lock<std::mutex> lock(mutex);
            callback_entered = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return callback_may_return; });
        });
        {
            std::lock_guard<std::mutex> lock(mutex);
            server_may_close = true;
            cv.notify_all();
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return callback_entered; }),
                  "TLS disconnect callback did not start");
        }
        const int waits_before = fake_delay_calls.load();
        std::atomic_bool destroyed{false};
        std::thread owner([tls = std::move(tls), &destroyed]() mutable {
            tls.reset();
            destroyed = true;
        });
        Check(WaitUntil([&]() { return fake_delay_calls.load() > waits_before; }),
              "TLS destructor did not wait for receive callback");
        Check(!destroyed.load(), "TLS state freed during receive callback");
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback_may_return = true;
            cv.notify_all();
        }
        owner.join();
        server.join();
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::atomic_bool server_saw_shutdown{false};
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "TLS write accept failed");
            char byte;
            recv(peer, &byte, 1, 0);
            server_saw_shutdown = true;
            close(peer);
            close(listener);
        });
        auto tls = std::make_unique<EspSsl>();
        Check(tls->Connect("127.0.0.1", port).has_value(), "TLS write connect failed");
        fake_tls_block_write = true;
        fake_tls_write_started = false;
        fake_tls_allow_write = false;
        EspSsl* raw_tls = tls.get();
        std::thread sender([&]() { raw_tls->Send("test"); });
        Check(WaitUntil([&]() { return fake_tls_write_started.load(); }),
              "TLS write did not enter protected section");
        const int destroys_before = fake_tls_destroy_count.load();
        std::atomic_bool destroyed{false};
        std::thread owner([tls = std::move(tls), &destroyed]() mutable {
            tls.reset();
            destroyed = true;
        });
        Check(WaitUntil([&]() { return server_saw_shutdown.load(); }),
              "TLS disconnect did not shutdown socket");
        Check(!destroyed.load(), "TLS object freed during write");
        Check(fake_tls_destroy_count.load() == destroys_before,
              "TLS session destroyed before in-flight write finished");
        fake_tls_allow_write = true;
        sender.join();
        owner.join();
        server.join();
        fake_tls_block_write = false;
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::mutex mutex;
        std::condition_variable cv;
        bool peer_ready = false;
        bool allow_close = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "interrupt TCP accept failed");
            const int receive_buffer = 4096;
            Check(setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                             sizeof(receive_buffer)) == 0,
                  "interrupt TCP receive buffer setup failed");
            std::unique_lock<std::mutex> lock(mutex);
            peer_ready = true;
            cv.notify_all();
            cv.wait(lock, [&]() { return allow_close; });
            close(peer);
            close(listener);
        });
        EspTcp tcp;
        Check(tcp.Connect("127.0.0.1", port).has_value(), "interrupt TCP connect failed");
        {
            std::unique_lock<std::mutex> lock(mutex);
            Check(cv.wait_for(lock, std::chrono::seconds(2), [&]() { return peer_ready; }),
                  "interrupt TCP peer did not start");
        }
        std::atomic_bool sender_finished{false};
        std::atomic_int send_result{0};
        std::thread sender([&]() {
            send_result = tcp.Send(std::string(8 * 1024 * 1024, 'x'));
            sender_finished = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Check(!sender_finished.load(), "TCP send did not reach backpressure");
        const auto started = std::chrono::steady_clock::now();
        tcp.Interrupt();
        Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
              "TCP Interrupt waited for sender or receive task");
        Check(WaitUntil([&]() { return sender_finished.load(); }),
              "TCP Interrupt did not release sender");
        sender.join();
        Check(send_result == -1, "interrupted TCP send unexpectedly succeeded");
        {
            std::lock_guard<std::mutex> lock(mutex);
            allow_close = true;
            cv.notify_all();
        }
        tcp.Disconnect();
        server.join();
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::mutex mutex;
        std::condition_variable cv;
        bool allow_close = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "WANT_WRITE TLS accept failed");
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&]() { return allow_close; });
            close(peer);
            close(listener);
        });
        EspSsl tls;
        Check(tls.Connect("127.0.0.1", port).has_value(), "WANT_WRITE TLS connect failed");
        fake_tls_write_started = false;
        fake_tls_want_write = true;
        const int delays_before = fake_delay_calls.load();
        const auto started = std::chrono::steady_clock::now();
        Check(tls.Send("test") == -1, "WANT_WRITE TLS send did not reach deadline");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        Check(elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(8),
              "WANT_WRITE TLS send missed its bounded deadline");
        Check(fake_tls_write_started.load() && fake_delay_calls.load() > delays_before,
              "WANT_WRITE TLS send did not yield CPU");
        fake_tls_want_write = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            allow_close = true;
            cv.notify_all();
        }
        tls.Disconnect();
        server.join();
    }
    {
        uint16_t port;
        const int listener = StartListener(port);
        std::mutex mutex;
        std::condition_variable cv;
        bool allow_close = false;
        std::thread server([&]() {
            const int peer = accept(listener, nullptr, nullptr);
            Check(peer >= 0, "interrupt TLS accept failed");
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&]() { return allow_close; });
            close(peer);
            close(listener);
        });
        EspSsl tls;
        Check(tls.Connect("127.0.0.1", port).has_value(), "interrupt TLS connect failed");
        fake_tls_write_started = false;
        fake_tls_want_write = true;
        std::atomic_bool sender_finished{false};
        std::atomic_int send_result{0};
        std::thread sender([&]() {
            send_result = tls.Send("test");
            sender_finished = true;
        });
        Check(WaitUntil([&]() { return fake_tls_write_started.load(); }),
              "interrupt TLS write did not start");
        const auto started = std::chrono::steady_clock::now();
        tls.Interrupt();
        Check(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500),
              "TLS Interrupt waited for sender or receive task");
        Check(WaitUntil([&]() { return sender_finished.load(); }),
              "TLS Interrupt did not release sender");
        sender.join();
        Check(send_result == -1, "interrupted TLS send unexpectedly succeeded");
        fake_tls_want_write = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            allow_close = true;
            cv.notify_all();
        }
        tls.Disconnect();
        server.join();
    }
    std::cout << "24 TCP joins, 2 WebSocket, 1 HTTP and 2 TLS lifecycle cases passed" << std::endl;
    std::cout << "Zero-length HTTP keep-alive regression passed" << std::endl;
    std::cout << "TCP/TLS Interrupt and WANT_WRITE deadline passed" << std::endl;
}
