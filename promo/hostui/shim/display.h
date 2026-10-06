#pragma once
#include <memory>
#include <string>
#include <esp_log.h>
#include "lvgl.h"
class LvglFont; class LvglImage;
class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;
    std::string name() const { return name_; }
    virtual std::shared_ptr<LvglFont> GetTextFont() const { return nullptr; }
private:
    std::string name_;
};
class Display {
public:
    virtual ~Display() = default;
    virtual void SetStatus(const char*) {}
    virtual void ShowNotification(const char*, int = 3000) {}
    virtual void SetEmotion(const char*) {}
    virtual void SetChatMessage(const char*, const char*) {}
    virtual void ClearChatMessages() {}
    virtual void SetTheme(Theme* t) { current_theme_ = t; }
    virtual void SetupUI() { setup_ui_called_ = true; }
    int width() const { return width_; }
    int height() const { return height_; }
protected:
    int width_ = 1280, height_ = 720;
    bool setup_ui_called_ = false;
    Theme* current_theme_ = nullptr;
    friend class DisplayLockGuard;
    virtual bool Lock(int = 0) { return true; }
    virtual void Unlock() {}
};
class DisplayLockGuard {
public:
    explicit DisplayLockGuard(Display* d) : d_(d), locked_(d->Lock(30000)) {}
    ~DisplayLockGuard() { if (locked_) d_->Unlock(); }
    bool locked() const { return locked_; }
private:
    Display* d_; bool locked_;
};
