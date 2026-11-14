#pragma once

#include <stddef.h>

// A new TCP/TLS client needs a contiguous internal block for the socket and for
// DMA-capable mbedtls/lwIP metadata. Small default allocations already use
// MALLOC_CAP_INTERNAL under SPIRAM_MALLOC_ALWAYSINTERNAL, so they can fill the
// DMA reserve. Refuse the connect before esp-tls runs, and let the caller's
// existing poll interval try again. This does not disable the feature.
constexpr size_t kMinInternalFreeForConnect = 32 * 1024;
constexpr size_t kMinInternalLargestForConnect = 8 * 1024;

inline bool HeapAllowsNewConnection(size_t free_internal, size_t largest_internal) {
    return free_internal >= kMinInternalFreeForConnect &&
           largest_internal >= kMinInternalLargestForConnect;
}
