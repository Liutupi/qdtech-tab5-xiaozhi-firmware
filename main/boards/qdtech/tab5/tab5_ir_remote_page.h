#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include "lvgl.h"

// Universal learning IR remote: any number of devices (TV, AC, fan, ...), each with its own
// keys; every key can be learned from the original remote and sent again. Codes are stored
// by ir::Store (SD card + NVS backup). All LVGL calls happen with the display lock held.
class Tab5IrRemotePage {
public:
    explicit Tab5IrRemotePage(lv_obj_t* parent, std::function<void()> back);
    void Show();
    void Hide();

private:
    enum class Overlay { kNone, kAddDevice, kAddKey };

    lv_obj_t* root_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_obj_t* device_list_ = nullptr;
    lv_obj_t* device_title_ = nullptr;
    lv_obj_t* key_grid_ = nullptr;
    lv_obj_t* learn_btn_ = nullptr;
    lv_obj_t* learn_label_ = nullptr;
    lv_obj_t* delete_label_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
    std::function<void()> back_;

    int current_device_ = -1;
    bool learn_mode_ = false;
    int64_t delete_armed_us_ = 0;
    int pending_key_delete_ = -1;
    int64_t key_delete_armed_us_ = 0;
    std::atomic<bool> busy_{false};
    // Guided learning: the key currently being listened for, and a cancel flag for the
    // blocking receive. learn_gen_ invalidates queued learn jobs that were superseded.
    int learn_target_ = -1;
    std::atomic<bool> cancel_learn_{false};
    std::atomic<int> learn_gen_{0};
    bool checked_polarity_ = false;

    QueueHandle_t jobs_ = nullptr;
    struct Job {
        std::function<void()> run;
    };

    // Per-button callback payloads (owned by the page, rebuilt with the grid).
    struct Tag {
        Tab5IrRemotePage* page;
        int kind;
        int a;
        std::string text;
    };
    std::vector<Tag*> tags_;
    std::vector<Tag*> overlay_tags_;

    void Build();
    void RefreshDevices();  // device list + key grid
    void RefreshKeys();
    void DropTags(std::initializer_list<int> kinds);
    void SetStatus(const std::string& text);
    void SetStatusLocked(const std::string& text);
    void OpenOverlay(Overlay kind);
    void CloseOverlay();

    void OnKey(int key_id, bool long_press);
    void Learn(int key_id, const std::string& name, bool auto_advance);
    void StartGuidedLearning();
    void StopLearning();
    void Send(int key_id, const std::string& name);
    void SelfTest(bool interactive);
    void Post(std::function<void()> run);
    static void Worker(void* arg);

    lv_obj_t* MakeButton(lv_obj_t* parent, const std::string& text, int w, int h, uint32_t bg,
                         uint32_t fg, Tag* tag);
    Tag* NewTag(int kind, int a, const std::string& text, bool overlay = false);
    static void OnEvent(lv_event_t* e);
};
