#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace tab5_audio_stream {
inline uint64_t SourceKey(std::string_view url) {
    uint64_t key = 14695981039346656037ULL;
    for (unsigned char c : url)
        key = (key ^ c) * 1099511628211ULL;
    return key;
}
inline std::string Escape(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
inline std::string RelayUrl(std::string_view inbox_url, std::string_view host,
                            std::string_view source, bool live) {
    if (source.empty() || source.size() > 2048 ||
        (source.rfind("http://", 0) != 0 && source.rfind("https://", 0) != 0) ||
        source.find("/stream96") != std::string_view::npos)
        return {};
    std::string endpoint;
    if (inbox_url.empty()) {
        if (host.empty() || host.find('/') != std::string_view::npos)
            return {};
        endpoint = "http://" + std::string(host) + "/tab5/podcast/stream96";
    } else {
        const auto path = inbox_url.find("/inbox/");
        if (path == std::string_view::npos)
            return {};
        endpoint = inbox_url;
        endpoint.replace(path, 7, "/podcast/");
        endpoint += "/stream96";
    }
    return endpoint + "?url=" + Escape(source) + (live ? "&live=1" : "&live=0");
}
struct Range {
    bool valid = false;
    uint64_t first = 0, last = 0, total = 0;
};
inline Range ParseRange(std::string_view text) {
    Range out;
    if (text.size() > 80 || text.substr(0, 6) != "bytes ")
        return out;
    text.remove_prefix(6);
    auto number = [&](uint64_t& value, char separator) {
        size_t i = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            unsigned digit = unsigned(text[i++] - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
                return false;
            value = value * 10 + digit;
        }
        if (!i || (separator && (i >= text.size() || text[i] != separator)) ||
            (!separator && i != text.size()))
            return false;
        text.remove_prefix(i + (separator ? 1 : 0));
        return true;
    };
    out.valid = number(out.first, '-') && number(out.last, '/') && number(out.total, 0) &&
                out.first <= out.last && out.last < out.total && out.total <= 0x7fffffffULL;
    return out;
}
// Player-task owned. Controls invalidate it by advancing the stream generation.
struct ResumePoint {
    uint64_t source = 0, byte = 0;
    uint32_t generation = 0, milliseconds = 0;
    bool valid = false;
    std::array<uint64_t, 32> boundaries{};
    unsigned next = 0, count = 0;
    bool Matches(std::string_view url, uint32_t current) const {
        return valid && generation == current && source == SourceKey(url);
    }
    void Capture(std::string_view url, uint32_t current, uint64_t consumed, uint32_t ms) {
        if (!Matches(url, current)) {
            next = count = 0;
        }
        source = SourceKey(url);
        generation = current;
        byte = consumed;
        milliseconds = ms;
        valid = consumed > 0;
        boundaries[next] = consumed;
        next = (next + 1) % boundaries.size();
        count = std::min(count + 1, unsigned(boundaries.size()));
    }
    // Start at an observed MP3 frame boundary. An arbitrary byte offset can
    // match sync-like payload data before the decoder reservoir is refilled.
    uint64_t Start() const { return count == boundaries.size() ? boundaries[next] : 0; }
};
// Interleaved L/R at the existing codec clock. Mono sources duplicate into both
// slots; stereo sources are interpolated independently, never downmixed.
inline int ResampleStereo(const int16_t* input, int samples, int channels, int rate,
                          int output_rate, int16_t* output, int capacity,
                          uint64_t* phase = nullptr) {
    if (!input || !output || samples <= 0 || (channels != 1 && channels != 2) ||
        samples % channels || rate <= 0 || output_rate <= 0 || capacity < 2)
        return 0;
    const int frames = samples / channels;
    const uint64_t start = phase ? *phase : 0;
    const uint64_t end = uint64_t(frames) * unsigned(output_rate);
    if (start >= end)
        return 0;
    const int64_t count = (end - start + unsigned(rate) - 1) / unsigned(rate);
    if (count <= 0 || count > capacity / 2)
        return 0;
    for (int i = 0; i < count; ++i) {
        const uint64_t pos = (start + uint64_t(i) * unsigned(rate)) * 65536 / unsigned(output_rate);
        const int index = std::min(int(pos >> 16), frames - 1);
        const int next = std::min(index + 1, frames - 1);
        const int frac = int(pos & 0xffff);
        for (int c = 0; c < 2; ++c) {
            const int channel = channels == 1 ? 0 : c;
            const int64_t a = input[index * channels + channel];
            const int64_t b = input[next * channels + channel];
            output[i * 2 + c] = int16_t((a * (65536 - frac) + b * frac) >> 16);
        }
    }
    if (phase)
        *phase = start + uint64_t(count) * unsigned(rate) - end;
    return int(count);
}
}  // namespace tab5_audio_stream
