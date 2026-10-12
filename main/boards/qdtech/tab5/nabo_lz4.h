#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nabo_sd {
// Raw LZ4 block, bounded by both the encoded payload and exact decoded size.
// No allocation or filesystem access; used only by the low-priority SD worker.
inline bool DecodeLz4(const uint8_t* in, size_t bytes, uint8_t* out, size_t size) {
    size_t ip = 0, op = 0;
    auto length = [&](size_t& n) {
        if (n != 15)
            return true;
        uint8_t extra;
        do {
            if (ip == bytes)
                return false;
            extra = in[ip++];
            if (n > size || extra > size - n)
                return false;
            n += extra;
        } while (extra == 255);
        return true;
    };
    while (ip < bytes) {
        const uint8_t token = in[ip++];
        size_t literals = token >> 4;
        if (!length(literals) || literals > bytes - ip || literals > size - op)
            return false;
        std::memcpy(out + op, in + ip, literals);
        ip += literals;
        op += literals;
        if (ip == bytes)
            return op == size;
        if (bytes - ip < 2)
            return false;
        const size_t offset = in[ip] | size_t(in[ip + 1]) << 8;
        ip += 2;
        size_t match = token & 15;
        if (!offset || offset > op || !length(match) || size - op < 4 || match > size - op - 4)
            return false;
        match += 4;
        // LZ4 matches can overlap (e.g. one-byte runs), so memcpy/memmove is wrong.
        for (size_t i = 0; i < match; ++i)
            out[op + i] = out[op + i - offset];
        op += match;
    }
    return false;
}
}  // namespace nabo_sd
