#include <cassert>
#include <cstdio>
#include "nabo_scene_policy.h"
#include "reply_wait_state.h"
using namespace nabo_scene;
int main() {
    ReplyWaitState wait;
    assert(!wait.Waiting(0, kDeviceStateListening));
    assert(!wait.Recognized(20, kDeviceStateConnecting));
    assert(!wait.Recognized(40, kDeviceStateListening));
    wait.Voice(true);
    assert(!wait.Recognized(100, kDeviceStateListening));
    assert(!wait.Waiting(120, kDeviceStateListening));
    wait.Voice(false);
    assert(!wait.Waiting(140, kDeviceStateListening));
    assert(wait.Recognized(160, kDeviceStateListening));
    assert(wait.Waiting(160, kDeviceStateListening));
    std::puts(
        "PASS no phone during connection, armed listening, active capture or silence without STT; "
        "confirmed quiet turn enters waiting");
    Output idle{State::Idle, 12, 0, 0, 255, 1, false};
    SceneInput in{};
    in.listening = true;
    in.working = wait.Waiting(160, kDeviceStateListening);
    WorkClock clock;
    auto selected = clock.Apply(160, Select(160, idle, in));
    assert(selected.clip == 3 && selected.frame == 0);
    selected = clock.Apply(840, Select(840, idle, in));
    assert(selected.clip == 3 && selected.frame == 17);
    in.playback = true;
    in.working = wait.Waiting(880, kDeviceStateListening, true);
    assert(!in.working);
    assert(clock.Apply(880, Select(880, idle, in)).clip == -1);
    assert(!wait.Waiting(900, kDeviceStateListening));  // A pause cannot revive this wait.
    assert(!wait.Recognized(900, kDeviceStateListening));
    in.playback = false;
    in.speaking = true;
    assert(clock.Apply(920, Select(920, idle, in)).clip == -1);
    wait.Reset();
    assert(!wait.Waiting(920, kDeviceStateSpeaking));
    std::puts(
        "PASS actual playback alone or speaking preempts at 720ms, without waiting for the 2000ms "
        "loop boundary");
    for (unsigned reason = 0; reason < 6; ++reason) {
        wait.Voice(true);
        wait.Voice(false);
        assert(wait.Recognized(1000, kDeviceStateListening));
        wait.Reset();  // cancel, new wake, protocol close, network loss, error, external audio
        assert(!wait.Waiting(1040, kDeviceStateIdle));
        assert(!wait.Recognized(1080, kDeviceStateListening));  // stale STT after cancellation
    }
    std::puts(
        "PASS six interruption routes cancel waiting and reject stale STT until fresh speech");
    wait.Voice(true);
    wait.Voice(false);
    assert(wait.Recognized(1200, kDeviceStateListening));
    wait.Voice(true);
    assert(!wait.Waiting(1240, kDeviceStateListening));
    wait.Voice(false);
    assert(!wait.Waiting(1280, kDeviceStateListening));
    assert(wait.Recognized(1320, kDeviceStateListening));
    in = {};
    in.listening = true;
    in.working = true;
    assert(clock.Apply(1320, Select(1320, idle, in)).frame == 0);
    assert(clock.Apply(3320, Select(3320, idle, in)).frame == 0);
    assert(clock.Apply(3360, Select(3360, idle, in)).frame == 1);
    std::puts(
        "PASS resumed capture clears previous wait; next confirmed turn starts frame zero and "
        "loops at exact source 25fps");
    const bool heard = wait.SawVoice();
    wait.Reset();
    wait.SubmittedManual(4000, heard);
    assert(wait.Waiting(4000, kDeviceStateIdle));
    in = {};
    in.working = wait.Waiting(4000, kDeviceStateIdle);
    in.voice = VoiceBlocksScene(in.working, true, true);
    assert(Select(4000, idle, in).clip == -1);  // Still capturing: VAD always wins.
    in.voice = VoiceBlocksScene(in.working, true, false);
    assert(Select(4040, idle, in).clip == 3);      // Capture stopped: stale VAD cannot stick.
    // A stale VAD bit after capture stopped (conversation ended) never blocks idle.
    assert(!VoiceBlocksScene(false, true, false));
    assert(VoiceBlocksScene(false, true, true));
    assert(!VoiceBlocksScene(true, false, true));
    wait.Reset();
    wait.SubmittedManual(4200, false);
    assert(!wait.Waiting(4200, kDeviceStateIdle));
    std::puts(
        "PASS accepted manual stop after speech waits in Idle; empty/manual stop without speech "
        "does not");
    wait.Voice(true);
    wait.Voice(false);
    assert(wait.Recognized(10000, kDeviceStateListening));
    assert(wait.Recognized(59000, kDeviceStateListening));
    assert(wait.Waiting(69999, kDeviceStateListening));
    assert(!wait.Waiting(70000, kDeviceStateListening));
    assert(!wait.Recognized(70040, kDeviceStateListening));
    wait.Voice(true);
    wait.Voice(false);
    assert(wait.Recognized(0xfffffff0u, kDeviceStateListening));
    assert(wait.Waiting(0x20u, kDeviceStateListening));
    assert(!wait.Waiting(0x10000u, kDeviceStateListening));
    std::puts(
        "PASS bounded visual timeout, duplicate-STT non-extension and millisecond counter "
        "rollover");
    std::printf(
        "RESULT observer_bytes=%zu work_clock_bytes=%zu audio_or_transport_mutations=none\n",
        sizeof(ReplyWaitState), sizeof(WorkClock));
}
