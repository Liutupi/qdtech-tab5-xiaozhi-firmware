#pragma once
#include <algorithm>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

// Playback cushion and poll policy for the Tab5 96 kbps stereo relay.
// Live ingress is paced near realtime, so a larger ring cannot invent bytes the
// socket never delivered. These decisions keep the existing fidelity, refuse to
// start on a few KB, and keep tunnel handshakes away from an empty cushion.
// They do not change partitions, NVS, or the Wi-Fi power-save level.

namespace tab5_playback {

inline constexpr int kStreamBitrateKbps = 96;
inline constexpr int kPlaybackReaderPriority = 11;
// application.cc raises the main task to 10. The socket reader must preempt it.
inline constexpr int kApplicationMainPriority = 10;
inline constexpr uint32_t kHealthyPollBytes = 64 * 1024;
inline constexpr char kPlaybackPatchId[] = "tab5-playback-20261108a";

static_assert(kPlaybackReaderPriority > kApplicationMainPriority, "reader would stay below main");
static_assert(kStreamBitrateKbps == 96, "repair must keep 96 kbps stereo");

enum class AudioPath { None, Lan, Tunnel };

// LAN is preferred only after a recent success. A configured tunnel stays available
// whenever LAN is not the path in use, including after a LAN open failure.
inline bool UseLanRelay(bool have_tunnel, bool have_host, bool lan_known) {
    if (!have_host) return false;
    if (!have_tunnel) return true;
    return lan_known;
}

inline AudioPath ChooseAudioPath(bool have_tunnel, bool have_host, bool lan_known) {
    if (UseLanRelay(have_tunnel, have_host, lan_known)) return AudioPath::Lan;
    if (have_tunnel) return AudioPath::Tunnel;
    return AudioPath::None;
}

inline bool LanFailureFallsBackToTunnel(bool have_tunnel, bool have_host, bool lan_known) {
    return UseLanRelay(have_tunnel, have_host, lan_known) && have_tunnel;
}

inline bool IsRelayStreamRequest(std::string_view url) {
    return url.find("/stream96") != std::string_view::npos;
}

// Direct provider sockets keep Connection: close. The relay hop is one long
// response and should not advertise close on every stream96 request.
inline const char* AudioConnectionHeader(std::string_view playback_url) {
    return IsRelayStreamRequest(playback_url) ? "keep-alive" : "close";
}

enum class FillKind { Start, Rebuffer };
enum class FillAction { Wait, ResumeAtTarget, ResumeBelowTarget, Cancelled };

struct StreamBufferPlan {
    size_t start_target = 0;
    size_t start_minimum = 0;
    size_t rebuffer_target = 0;
    size_t rebuffer_minimum = 0;
    size_t low_water = 0;
    int64_t start_budget_us = 0;
    int64_t start_extend_us = 0;
    int64_t rebuffer_budget_us = 0;
    int64_t rebuffer_extend_us = 0;
};

struct FillConfig {
    size_t target = 0;
    size_t minimum = 0;
    int64_t budget_us = 0;
    int64_t extend_us = 0;
};

struct FillView {
    size_t have = 0;
    int64_t elapsed_us = 0;
    bool reader_running = false;
    bool cancel = false;
};

// Live cannot run ahead of the encoder, so its cushion is a few seconds.
// Cached songs can, so the start target uses more of the existing ring.
inline StreamBufferPlan BufferPlan(bool live) {
    constexpr int64_t kSecond = 1000000;
    if (live) {
        return {48 * 1024, 16 * 1024, 32 * 1024, 12 * 1024, 6 * 1024,
                8 * kSecond, 4 * kSecond, 6 * kSecond, 4 * kSecond};
    }
    return {96 * 1024, 24 * 1024, 48 * 1024, 16 * 1024, 8 * 1024,
            8 * kSecond, 4 * kSecond, 6 * kSecond, 4 * kSecond};
}

inline FillConfig FillConfigFor(const StreamBufferPlan& plan, FillKind kind, size_t capacity) {
    FillConfig cfg;
    if (kind == FillKind::Start) {
        cfg = {plan.start_target, plan.start_minimum, plan.start_budget_us, plan.start_extend_us};
    } else {
        cfg = {plan.rebuffer_target, plan.rebuffer_minimum, plan.rebuffer_budget_us,
               plan.rebuffer_extend_us};
    }
    if (capacity > 0) {
        const size_t capped = capacity - capacity / 4;
        if (capped > 0 && cfg.target > capped) cfg.target = capped;
        if (cfg.minimum > cfg.target) cfg.minimum = cfg.target;
    }
    return cfg;
}

inline FillAction DecideFill(const FillConfig& cfg, const FillView& view) {
    if (view.cancel) return FillAction::Cancelled;
    if (cfg.target > 0 && view.have >= cfg.target) return FillAction::ResumeAtTarget;
    if (!view.reader_running)
        return view.have > 0 ? FillAction::ResumeBelowTarget : FillAction::Cancelled;
    const int64_t limit = cfg.budget_us + (view.have < cfg.minimum ? cfg.extend_us : 0);
    if (view.elapsed_us < limit) return FillAction::Wait;
    return FillAction::ResumeBelowTarget;
}

inline bool ShouldRebuffer(size_t buffered, size_t low_water, bool output_started,
                           bool reader_running) {
    return output_started && reader_running && buffered < low_water;
}

// Tunnel TLS is allowed during playback only after output has started and the
// cushion can absorb a multi-second handshake. Otherwise the poll waits.
inline bool AllowBackgroundTunnelPoll(bool audio_active, bool output_started, uint32_t buffered) {
    if (!audio_active) return true;
    return output_started && buffered >= kHealthyPollBytes;
}

inline int BackgroundPollWaitMs(bool output_started, uint32_t buffered) {
    if (!output_started || buffered < kHealthyPollBytes) return 2000;
    return 20000;
}

enum class MusicPollRoute { Skip, LanOnly, TunnelOnly, ProbeLanThenTunnel };

inline MusicPollRoute ChooseMusicPoll(bool audio_active, bool allow_poll, bool lan_known,
                                      bool lan_probe_due, bool have_tunnel, bool have_host) {
    if (!have_tunnel && !have_host) return MusicPollRoute::Skip;
    // A known LAN hop is plain HTTP on the local relay. Keep it during playback.
    // Tunnel TLS is the handshake that stalls the audio socket, so it waits
    // for a healthy cushion and always remains the fallback when LAN is down.
    if (lan_known && have_host) return MusicPollRoute::LanOnly;
    if (audio_active && !allow_poll) return MusicPollRoute::Skip;
    if (audio_active)
        return have_tunnel ? MusicPollRoute::TunnelOnly : MusicPollRoute::LanOnly;
    if (have_host && (lan_probe_due || !have_tunnel))
        return have_tunnel ? MusicPollRoute::ProbeLanThenTunnel : MusicPollRoute::LanOnly;
    if (have_tunnel) return MusicPollRoute::TunnelOnly;
    return MusicPollRoute::LanOnly;
}

inline int RecvKbps(size_t bytes, int64_t interval_us) {
    if (interval_us <= 0 || bytes == 0) return 0;
    return static_cast<int>((static_cast<uint64_t>(bytes) * 8000ULL) /
                            static_cast<uint64_t>(interval_us));
}

inline const char* RequestMode(bool live, bool lan) {
    if (live) return lan ? "lan-live" : "tunnel-live";
    return lan ? "lan-file" : "tunnel-file";
}

struct PlaybackDiag {
    size_t buffered = 0;
    size_t ring = 0;
    int local = 0;
    size_t received_bytes = 0;
    int64_t interval_us = 0;
    int consumer_ms = 0;
    int underruns = 0;
    uint32_t internal_free = 0;
    uint32_t psram_free = 0;
    uint32_t psram_largest = 0;
    int rcvbuf = -1;
    bool live = true;
    bool lan = false;
    const char* event = "play";
};

inline bool DiagTextSafe(std::string_view text) {
    return text.find("http://") == std::string_view::npos &&
           text.find("https://") == std::string_view::npos &&
           text.find("token") == std::string_view::npos &&
           text.find("sig=") == std::string_view::npos &&
           text.find('?') == std::string_view::npos;
}

// One line, no URL and no query. Returns 0 and clears the buffer when the
// formatted text would carry request material.
inline int FormatPlaybackDiag(char* out, size_t cap, const PlaybackDiag& diag) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    const char* event = diag.event ? diag.event : "play";
    if (!DiagTextSafe(event)) return 0;
    const int wrote = std::snprintf(
        out, cap,
        "playback patch=%s mode=%s event=%s recv_kbps=%d need_kbps=%d buffered=%u ring=%u "
        "local=%d consumer_ms=%d underruns=%d heap_i=%u heap_p=%u heap_pl=%u rcvbuf=%d",
        kPlaybackPatchId, RequestMode(diag.live, diag.lan), event,
        RecvKbps(diag.received_bytes, diag.interval_us), kStreamBitrateKbps,
        static_cast<unsigned>(diag.buffered), static_cast<unsigned>(diag.ring), diag.local,
        diag.consumer_ms, diag.underruns, static_cast<unsigned>(diag.internal_free),
        static_cast<unsigned>(diag.psram_free), static_cast<unsigned>(diag.psram_largest),
        diag.rcvbuf);
    if (wrote <= 0 || !DiagTextSafe(out)) {
        out[0] = '\0';
        return 0;
    }
    return wrote;
}

inline std::atomic<uint32_t>& PublishedBufferedBytes() {
    static std::atomic<uint32_t> bytes{0};
    return bytes;
}

inline std::atomic<uint8_t>& PublishedOutputStarted() {
    static std::atomic<uint8_t> started{0};
    return started;
}

inline void PublishPlaybackBuffer(size_t buffered, bool active, bool output_started) {
    // size_t is 32-bit on the ESP32-P4: a direct `buffered > UINT32_MAX` is an
    // always-false comparison there (-Werror=type-limits). Clamp in 64 bits instead.
    const uint32_t stored = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(buffered), UINT32_MAX));
    PublishedBufferedBytes().store(stored, std::memory_order_relaxed);
    PublishedOutputStarted().store(active && output_started ? 1 : 0, std::memory_order_relaxed);
    if (!active) PublishedBufferedBytes().store(0, std::memory_order_relaxed);
}

}  // namespace tab5_playback
