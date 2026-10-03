// Exercise the real WebSocket parser and URL handling with a controllable TCP peer.
#include <sdkconfig.h>
#include "network_interface.h"
#include "web_socket.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

NetworkError NetworkError::FromErrno(int error) { return ConnectFailed(error); }
NetworkError NetworkError::FromHErrno(int error) { return DnsFailed(error); }
NetworkError NetworkError::FromEsp(esp_err_t error) { return TlsFailed(error); }
std::string NetworkError::ToString() const { return "test error"; }

#if CONFIG_SPIRAM_USE_MALLOC
constexpr size_t kExpectedMessageCap = 128 * 1024;
#else
constexpr size_t kExpectedMessageCap = 16 * 1024;
#endif

static void Check(bool okay, const char* message) {
    if (!okay) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

class InputTcp;
struct PeerState {
    InputTcp* tcp = nullptr;
    std::string host;
    std::string request;
    std::string handshake = "HTTP/1.1 101 Switching Protocols\r\n\r\n";
    int port = 0;
    int creates = 0;
    int disconnects = 0;
    bool used_ssl = false;
    bool interrupted = false;
    void Emit(const std::string& data);
};

class InputTcp final : public Tcp {
public:
    explicit InputTcp(PeerState& peer) : peer_(peer) { peer_.tcp = this; }
    ~InputTcp() override { peer_.tcp = nullptr; }

    NetworkResult<> Connect(const std::string& host, int port) override {
        peer_.host = host;
        peer_.port = port;
        connected_ = true;
        return {};
    }
    void Disconnect() override { Interrupt(); }
    void Interrupt() override {
        if (!connected_.exchange(false))
            return;
        peer_.interrupted = true;
        ++peer_.disconnects;
        if (disconnect_callback_)
            disconnect_callback_();
    }
    int Send(const std::string& data) override {
        if (!connected_)
            return -1;
        if (data.starts_with("GET ")) {
            peer_.request = data;
            stream_callback_(peer_.handshake);
        }
        return static_cast<int>(data.size());
    }
    void Emit(const std::string& data) {
        if (connected_ && stream_callback_)
            stream_callback_(data);
    }

private:
    PeerState& peer_;
};

void PeerState::Emit(const std::string& data) {
    Check(tcp != nullptr, "TCP peer missing");
    tcp->Emit(data);
}

class TestNetwork final : public NetworkInterface {
public:
    explicit TestNetwork(PeerState& peer) : peer_(peer) {}
    std::unique_ptr<Http> CreateHttp(int) override { return nullptr; }
    std::unique_ptr<Tcp> CreateTcp(int) override {
        ++peer_.creates;
        return std::make_unique<InputTcp>(peer_);
    }
    std::unique_ptr<Tcp> CreateSsl(int) override {
        peer_.used_ssl = true;
        return CreateTcp(0);
    }
    std::unique_ptr<Udp> CreateUdp(int) override { return nullptr; }
    std::unique_ptr<Mqtt> CreateMqtt(int) override { return nullptr; }
    std::unique_ptr<WebSocket> CreateWebSocket(int) override { return nullptr; }

private:
    PeerState& peer_;
};

static std::string Frame(uint8_t first, std::string_view payload) {
    std::string frame(1, static_cast<char>(first));
    const size_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<char>(size));
    } else if (size <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<char>(size >> 8));
        frame.push_back(static_cast<char>(size));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>(static_cast<uint64_t>(size) >> shift));
    }
    frame.append(payload);
    return frame;
}

int main() {
    for (const char* invalid : std::array<const char*, 12>{
             nullptr, "", "ws://", "ftp://example.test/a", "ws://example.test:abc/a",
             "ws://example.test:0/a", "ws://example.test:65536/a", "ws://example.test:80x/a",
             "ws://[::1]:80/a", "ws://name@host/a", "ws://example.test/a#fragment",
             "ws://example.test/a\r\nInjected: yes"}) {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(!socket.Connect(invalid).has_value(), "malformed URL accepted");
        Check(peer.creates == 0, "malformed URL created a transport");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(socket.Connect("ws://example.test/chat:room?x=1").has_value(),
              "colon in path broke URL parsing");
        Check(peer.host == "example.test" && peer.port == 80, "default WS authority wrong");
        Check(peer.request.starts_with("GET /chat:room?x=1 HTTP/1.1\r\n"), "WS path or query lost");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(socket.Connect("wss://example.test:8443?topic=a").has_value(),
              "explicit WSS port or no-path query failed");
        Check(peer.used_ssl && peer.host == "example.test" && peer.port == 8443,
              "WSS authority wrong");
        Check(peer.request.starts_with("GET /?topic=a HTTP/1.1\r\n"), "query path wrong");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(socket.Connect("https://example.test/a").has_value(),
              "previous HTTPS transport support lost");
        Check(peer.used_ssl && peer.port == 443, "HTTPS did not use TLS default");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        size_t received_size = 0;
        socket.OnData([&](const char*, size_t size, bool binary) {
            Check(!binary, "text frame became binary");
            received_size = size;
        });
        Check(socket.Connect("ws://example.test/").has_value(), "normal connection failed");
        const size_t valid_size = kExpectedMessageCap;
        peer.Emit(Frame(0x81, std::string(valid_size, 'x')));
        Check(received_size == valid_size && !peer.interrupted,
              "valid unfragmented frame not delivered");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        size_t received_size = 0;
        int deliveries = 0;
        socket.OnData([&](const char*, size_t size, bool binary) {
            Check(!binary, "text frame became binary");
            received_size = size;
            ++deliveries;
        });
        Check(socket.Connect("ws://example.test/").has_value(), "normal connection failed");
        const size_t valid_first = kExpectedMessageCap / 4;
        const size_t valid_second = kExpectedMessageCap / 8;
        peer.Emit(Frame(0x01, std::string(valid_first, 'a')));
        peer.Emit(Frame(0x80, std::string(valid_second, 'b')));
        Check(deliveries == 1 && received_size == valid_first + valid_second,
              "valid fragmented message not delivered once");
        Check(!peer.interrupted, "valid message interrupted connection");
        peer.Emit(Frame(0x01, std::string(kExpectedMessageCap * 3 / 4, 'a')));
        peer.Emit(Frame(0x80, std::string(kExpectedMessageCap / 2, 'b')));
        Check(peer.interrupted && !socket.IsConnected(),
              "oversized fragmented message did not disconnect");
        Check(deliveries == 1, "oversized fragmented message was delivered");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(socket.Connect("ws://example.test/").has_value(), "normal connection failed");
        std::string header =
            Frame(0x81, std::string(kExpectedMessageCap + 1024, 'x')).substr(0, 10);
        peer.Emit(header);
        Check(peer.interrupted && !socket.IsConnected(),
              "advertised oversized frame waited for body");
    }
    {
        PeerState peer;
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(socket.Connect("ws://example.test/").has_value(), "normal connection failed");
        peer.Emit(Frame(0x88, ""));
        Check(peer.interrupted && !socket.IsConnected(),
              "peer close frame left a live TCP connection");
    }
    {
        PeerState peer;
        peer.handshake = "HTTP/1.1 101 Switching Protocols\r\nX: " + std::string(8 * 1024, 'x');
        TestNetwork network(peer);
        WebSocket socket(&network, 1);
        Check(!socket.Connect("ws://example.test/").has_value(),
              "oversized handshake unexpectedly succeeded");
        Check(peer.interrupted, "oversized handshake did not interrupt TCP");
    }
    std::cout << "WebSocket URL and receive limits passed" << std::endl;
}
