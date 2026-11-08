"""Host checks for the Tab5 playback cushion, route fallback and redacted diagnostics."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


@unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
class PlaybackBufferTests(unittest.TestCase):
    def test_cushion_route_poll_and_redacted_diag(self):
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include "tab5_playback_buffer.h"

namespace {

void Step(size_t* have, size_t capacity, size_t produced, size_t* played, bool* filling,
          tab5_playback::FillKind* kind, int64_t* fill_elapsed, int* underruns, bool* cancelled,
          const tab5_playback::StreamBufferPlan& plan, size_t consume, bool reader, bool cancel,
          int64_t dt) {
    using namespace tab5_playback;
    *have = std::min(capacity, *have + produced);
    if (*filling) {
        *fill_elapsed += dt;
        const auto action = DecideFill(FillConfigFor(plan, *kind, capacity),
                                        {*have, *fill_elapsed, reader, cancel});
        if (action == FillAction::Cancelled) {
            *cancelled = true;
            *filling = false;
            return;
        }
        if (action == FillAction::Wait) return;
        *filling = false;
        *fill_elapsed = 0;
        return;
    }
    if (cancel) {
        *cancelled = true;
        return;
    }
    const size_t eat = std::min(*have, consume);
    *have -= eat;
    *played += eat;
    if (ShouldRebuffer(*have, plan.low_water, true, reader)) {
        ++*underruns;
        *filling = true;
        *kind = FillKind::Rebuffer;
        *fill_elapsed = 0;
    }
}

}  // namespace

int main() {
    using namespace tab5_playback;
    assert(kPlaybackReaderPriority > kApplicationMainPriority);
    assert(kStreamBitrateKbps == 96);
    assert(std::string(kPlaybackPatchId).find("tab5-playback-") == 0);

    const auto live = BufferPlan(true);
    const auto file = BufferPlan(false);
    assert(file.start_target > live.start_target);
    assert(live.start_minimum >= 16 * 1024);
    assert(live.low_water >= 6 * 1024);
    assert(std::strcmp(AudioConnectionHeader("https://relay.example/podcast/tok/stream96?url=x"),
                       "keep-alive") == 0);
    assert(std::strcmp(AudioConnectionHeader("https://cdn.example/live.mp3"), "close") == 0);
    assert(!IsRelayStreamRequest("https://cdn.example/live.mp3"));

    assert(ChooseAudioPath(true, true, true) == AudioPath::Lan);
    assert(LanFailureFallsBackToTunnel(true, true, true));
    assert(ChooseAudioPath(true, true, false) == AudioPath::Tunnel);
    assert(!LanFailureFallsBackToTunnel(true, true, false));
    assert(ChooseAudioPath(false, true, false) == AudioPath::Lan);
    assert(!LanFailureFallsBackToTunnel(false, true, false));
    assert(ChooseAudioPath(false, false, false) == AudioPath::None);
    assert(!UseLanRelay(true, false, true));

    assert(ChooseMusicPoll(true, false, true, false, true, true) == MusicPollRoute::LanOnly);
    assert(ChooseMusicPoll(true, false, false, true, true, true) == MusicPollRoute::Skip);
    assert(ChooseMusicPoll(true, true, false, true, true, true) == MusicPollRoute::TunnelOnly);
    assert(ChooseMusicPoll(false, true, false, true, true, true) == MusicPollRoute::ProbeLanThenTunnel);
    assert(ChooseMusicPoll(false, true, false, false, true, true) == MusicPollRoute::TunnelOnly);
    assert(ChooseMusicPoll(false, true, false, true, false, true) == MusicPollRoute::LanOnly);
    assert(ChooseMusicPoll(false, true, false, false, false, false) == MusicPollRoute::Skip);
    assert(!AllowBackgroundTunnelPoll(true, false, 96 * 1024));
    assert(!AllowBackgroundTunnelPoll(true, true, 48 * 1024));
    assert(AllowBackgroundTunnelPoll(true, true, kHealthyPollBytes));
    assert(AllowBackgroundTunnelPoll(false, false, 0));
    assert(BackgroundPollWaitMs(false, 0) == 2000);
    assert(BackgroundPollWaitMs(true, kHealthyPollBytes) == 20000);

    assert(RecvKbps(12000, 1000000) == 96);
    assert(RecvKbps(2432, 1000000) == 19);
    assert(RecvKbps(36864, 8000000) == 36);
    assert(RecvKbps(0, 1000000) == 0);

    const auto start = FillConfigFor(live, FillKind::Start, 128 * 1024);
    assert(DecideFill(start, {36 * 1024, 7 * 1000000, true, false}) == FillAction::Wait);
    assert(DecideFill(start, {36 * 1024, 8 * 1000000, true, false}) == FillAction::ResumeBelowTarget);
    assert(DecideFill(start, {48 * 1024, 1000000, true, false}) == FillAction::ResumeAtTarget);
    const auto rebuffer = FillConfigFor(live, FillKind::Rebuffer, 128 * 1024);
    assert(DecideFill(rebuffer, {1902, 6 * 1000000, true, false}) == FillAction::Wait);
    assert(DecideFill(rebuffer, {1902, 10 * 1000000, true, false}) == FillAction::ResumeBelowTarget);
    assert(DecideFill(rebuffer, {0, 1000, false, false}) == FillAction::Cancelled);
    assert(DecideFill(rebuffer, {20 * 1024, 1000, false, false}) == FillAction::ResumeBelowTarget);
    assert(DecideFill(start, {80 * 1024, 0, true, true}) == FillAction::Cancelled);
    const auto tight = FillConfigFor(live, FillKind::Start, 40 * 1024);
    assert(tight.target <= 30 * 1024);
    assert(tight.minimum <= tight.target);
    assert(ShouldRebuffer(5 * 1024, live.low_water, true, true));
    assert(!ShouldRebuffer(32 * 1024, live.low_water, true, true));
    assert(!ShouldRebuffer(0, live.low_water, false, true));
    assert(!ShouldRebuffer(0, live.low_water, true, false));

    size_t have = 0, played = 0;
    bool filling = true, cancelled = false;
    auto kind = FillKind::Start;
    int64_t elapsed = 0;
    int underruns = 0;
    for (int i = 0; i < 300; ++i)
        Step(&have, 128 * 1024, 1200, &played, &filling, &kind, &elapsed, &underruns, &cancelled,
             live, 1200, true, false, 100000);
    assert(!cancelled && underruns == 0 && played > 200000 && have >= live.low_water);

    have = played = 0;
    filling = true;
    cancelled = false;
    kind = FillKind::Start;
    elapsed = 0;
    underruns = 0;
    for (int i = 0; i < 400; ++i)
        Step(&have, 128 * 1024, 240, &played, &filling, &kind, &elapsed, &underruns, &cancelled,
             live, 1200, true, false, 100000);
    assert(underruns >= 2 && played > 0 && played < 200000);

    have = 0;
    filling = true;
    cancelled = false;
    kind = FillKind::Start;
    elapsed = 0;
    underruns = 0;
    played = 0;
    Step(&have, 128 * 1024, 1000, &played, &filling, &kind, &elapsed, &underruns, &cancelled, live,
         1200, true, true, 100000);
    assert(cancelled && played == 0);

    PlaybackDiag diag;
    diag.buffered = 36551;
    diag.ring = 0;
    diag.local = 36551;
    diag.received_bytes = 2432;
    diag.interval_us = 1000000;
    diag.consumer_ms = 1674;
    diag.underruns = 3;
    diag.internal_free = 39919;
    diag.psram_free = 362448;
    diag.psram_largest = 327680;
    diag.rcvbuf = 5744;
    diag.live = true;
    diag.lan = false;
    diag.event = "play";
    char line[384];
    assert(FormatPlaybackDiag(line, sizeof(line), diag) > 0);
    const std::string text(line);
    assert(text.find("patch=tab5-playback-") != std::string::npos);
    assert(text.find("mode=tunnel-live") != std::string::npos);
    assert(text.find("recv_kbps=19") != std::string::npos);
    assert(text.find("need_kbps=96") != std::string::npos);
    assert(text.find("buffered=36551") != std::string::npos);
    assert(text.find("ring=0") != std::string::npos);
    assert(text.find("consumer_ms=1674") != std::string::npos);
    assert(text.find("heap_i=39919") != std::string::npos);
    assert(text.find("heap_p=362448") != std::string::npos);
    assert(text.find("rcvbuf=5744") != std::string::npos);
    assert(text.find("http") == std::string::npos);
    assert(text.find("token") == std::string::npos);
    assert(text.find('?') == std::string::npos);
    diag.event = "http://relay.example/podcast/token/stream96?url=sig=secret";
    assert(FormatPlaybackDiag(line, sizeof(line), diag) == 0);
    assert(line[0] == '\0');

    PublishPlaybackBuffer(70 * 1024, true, true);
    assert(PublishedBufferedBytes().load() == 70 * 1024);
    assert(PublishedOutputStarted().load() == 1);
    PublishPlaybackBuffer(10 * 1024, true, false);
    assert(PublishedOutputStarted().load() == 0);
    PublishPlaybackBuffer(10 * 1024, false, true);
    assert(PublishedBufferedBytes().load() == 0);
    assert(PublishedOutputStarted().load() == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "test.cc"
            exe = Path(directory) / "test"
            cpp.write_text(source)
            result = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD), str(cpp),
                 "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)


if __name__ == "__main__":
    unittest.main()
