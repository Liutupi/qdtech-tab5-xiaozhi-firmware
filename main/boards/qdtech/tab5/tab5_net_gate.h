#pragma once

#include <mutex>

// One background HTTP(S) transfer at a time. Each concurrent connection holds lwIP receive
// buffers and client objects in scarce internal RAM: the Muse inbox and the 米家中控 poll
// overlapping right after boot drove the internal minimum from ~8.5 KB to under 1 KB.
namespace tab5_net {
inline std::mutex& TransferGate() {
    static std::mutex gate;
    return gate;
}
}  // namespace tab5_net
