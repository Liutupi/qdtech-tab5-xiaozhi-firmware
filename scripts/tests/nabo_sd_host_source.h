#pragma once
#include <chrono>
#include <cstdio>
#include "nabo_framepack.h"

namespace nabo_sd {
// One worker owns this object; never read it concurrently from two workers.
class FileSource : public Source {
public:
    explicit FileSource(const char* path) {
        file_ = std::fopen(path, "rb");
        if (!file_)
            return;
        std::setvbuf(file_, nullptr, _IONBF, 0);
        if (std::fseek(file_, 0, SEEK_END) != 0)
            return;
        const long size = std::ftell(file_);
        if (size >= 0)
            size_ = static_cast<uint64_t>(size);
    }
    ~FileSource() override {
        if (file_)
            std::fclose(file_);
    }
    uint64_t Size() const override { return size_; }
    size_t ReadAt(uint32_t offset, uint8_t* out, size_t bytes) override {
        if (!file_ || std::fseek(file_, offset, SEEK_SET) != 0)
            return 0;
        ++reads;
        requested_bytes += bytes;
        max_request = std::max(max_request, bytes);
        return std::fread(out, 1, bytes, file_);
    }
    uint64_t NowMs() const override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    uint64_t reads = 0, requested_bytes = 0;
    size_t max_request = 0;

private:
    std::FILE* file_ = nullptr;
    uint64_t size_ = 0;
};
}  // namespace nabo_sd
