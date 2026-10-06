#pragma once
#include <functional>
class AudioService {
public:
    bool IsPlaybackIdle() const;
    bool IsAudioProcessorRunning() const { return false; }
};
class Application {
public:
    static Application& GetInstance() { static Application a; return a; }
    void Schedule(std::function<void()> f);
    void ToggleChatState() {}
    bool IsWaitingForReply() const;
    bool IsVoiceDetected() const { return false; }
    void SetExternalAudioActive(bool) {}
    AudioService& GetAudioService() { return audio_; }
private:
    AudioService audio_;
};
