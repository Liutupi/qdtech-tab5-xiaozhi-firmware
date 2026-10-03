#pragma once

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "settings.h"

#ifdef CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT
#include "driver/ppa.h"
#include "esp_lcd_panel_ops.h"
#include "src/draw/sw/lv_draw_sw_utils.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "application.h"
#include "assets/lang_config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "font/binfont_loader/lv_binfont_loader.h"
#include "nabo_animation.h"
#include "nabo_assets.h"
#ifdef CONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT
#include "nabo_think_torso_raw.h"
#endif
#ifdef CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT
#include "nabo_portrait_matte.h"
#endif
#include "nabo_reactions.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "tab5_clinical_pearls.h"
#include "tab5_daily_content.h"
#include "tab5_frame_profile.h"
#include "tab5_home_animation.h"
#include "tab5_lrc.h"
#include "tab5_native_apps.h"
#include "tab5_radio_status_mailbox.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_cjk_28);
LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_clock_72);

// LVGL draws 1280x720 and the port rotates only dirty areas to ST7121 scanout.
class QdtechTab5Display : public MipiLcdDisplay {
#ifdef CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT
    // The DSI panel's existing on_color_trans_done callback completes each
    // partial LVGL flush. This buffer is shared by PPA and software rotation.
    // The board/display singleton keeps it until reboot because DMA2D copies
    // from it asynchronously. The verifier buffer is freed after four checks.
    static constexpr size_t kPpaMinimumPixels = 4096;
    static constexpr uint8_t kPpaVerifyRegions = 4;
    uint8_t* rotation_output_ = nullptr;
    uint8_t* verify_output_ = nullptr;
    size_t rotation_output_bytes_ = 0;
    ppa_client_handle_t ppa_client_ = nullptr;
    uint8_t verified_regions_ = 0;
    uint32_t verification_mismatches_ = 0;
    uint32_t ppa_attempts_ = 0;
    uint32_t ppa_successes_ = 0;
    uint32_t ppa_failures_ = 0;
    uint32_t ppa_consecutive_failures_ = 0;
    uint32_t sw_rotations_ = 0;
    uint32_t flush_errors_ = 0;
    uint32_t ppa_max_us_ = 0;
    uint64_t ppa_total_us_ = 0;
    bool ppa_disabled_ = false;
#endif
    lv_obj_t* portrait_ = nullptr;
#ifdef CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT
    bool portrait_matte_valid_ = false;
#endif
    lv_obj_t* blink_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* wave_ = nullptr;
    nabo::WaveGeometry pose_geometry_ = NaboWaveGeometry();
    // Automatic idle reactions use a few gentle whole-body key poses. The
    // large merged torso art is too costly to rotate on every UI tick.
    static constexpr uint16_t kIdleMajorKeysMs[] = {0, 180, 450, 900, 1350, 1700};
    bool pose_has_hand_ = true;
    bool pose_has_head_ = true;
    bool idle_pose_ = false;
    bool major_pose_valid_ = false;
    uint64_t pose_started_ms_ = 0;
    uint64_t last_major_pose_ms_ = 0;
    unsigned next_idle_major_key_ = 0;
    nabo::WavePose major_pose_;
    nabo::WaveMotion major_motion_;
    struct DisplayedPart {
        int x = 0;
        int y = 0;
        int angle = 0;
        bool valid = false;
    } displayed_pose_[5];
    lv_obj_t* wave_legs_ = nullptr;
    lv_obj_t* wave_torso_ = nullptr;
    lv_obj_t* wave_head_ = nullptr;
    lv_obj_t* wave_hand_ = nullptr;
    lv_obj_t* wave_cuff_ = nullptr;
    lv_obj_t* sleep_ = nullptr;
    lv_obj_t* ambient_[3] = {};
    lv_obj_t* sleep_z_[3] = {};
    lv_obj_t* daily_accent_ = nullptr;
    lv_obj_t* daily_title_label_ = nullptr;
    lv_obj_t* daily_body_label_ = nullptr;
    // Pages 0-2: pushed digest, 3-5: quote/history/festival, 6-8: offline clinical pearls.
    static constexpr unsigned kDailyPages = 9;
    lv_obj_t* daily_dots_[9] = {};
    // 0-2: pushed medical digest; 3-5: quote / history / festival.
    std::string daily_titles_[9];
    std::string daily_bodies_[9];
    std::string daily_sources_[9];
    struct DailyPersistSnapshot {
        int date_key = -1;
        std::string titles[3];
        std::string bodies[3];
    };
    std::atomic<bool> daily_persist_queued_{false};
    // A page turn owns one pending mirror update. It can run only after the
    // small daily card has completed a render, and any newer page replaces it.
    class DailyMirrorGate {
    public:
        void Queue(unsigned page, uint64_t revision, const char* prompt, const char* message) {
            pending_ = true;
            pending_revision_ = revision;
            pending_page_ = page;
            ready_after_frame_ = rendered_frames_ + 1;
            base_prompt_ = prompt ? prompt : "";
            base_message_ = message ? message : "";
        }

        void Cancel() { pending_ = false; }

        void RenderReady() { ++rendered_frames_; }
        bool HasPending() const { return pending_; }

        bool Take(unsigned page, uint64_t revision, bool idle, const char* prompt,
                  const char* message) {
            if (!HasPending())
                return false;
            if (!idle || revision != pending_revision_ || page != pending_page_ ||
                base_prompt_ != (prompt ? prompt : "") ||
                base_message_ != (message ? message : "")) {
                Cancel();
                return false;
            }
            if (rendered_frames_ < ready_after_frame_)
                return false;
            pending_ = false;
            return true;
        }

    private:
        bool pending_ = false;
        uint64_t pending_revision_ = 0;
        uint64_t rendered_frames_ = 0;
        uint64_t ready_after_frame_ = 0;
        unsigned pending_page_ = 0;
        std::string base_prompt_;
        std::string base_message_;
    } daily_mirror_gate_;
    bool daily_mirror_retry_on_home_ = false;
    std::string digest_prompt_ = "今日医学精选";
    unsigned next_page_tick_ = 0;
    int daily_date_key_ = -1;
    unsigned daily_page_ = 0;
    uint64_t daily_page_revision_ = 0;
    unsigned digest_count_ = 0;
    std::string digest_text_;
    bool has_digest_ = false;
    lv_font_t* sd_fallback_font_ = nullptr;
    tab5_home::Animation face_animation_;
    unsigned eye_frame_ = 0;
    lv_obj_t* thought_[3] = {};
    int bar_heights_[5] = {};
    nabo::Animation pose_animation_;
    uint64_t last_touch_ms_ = 0;
    uint64_t next_idle_reaction_ms_ = 0;
    unsigned touch_reaction_ = 0;
    unsigned idle_reaction_ = 0;
    nabo::PendingEmotion pending_emotion_;
    bool awaiting_reply_ = false;
    uint32_t greeting_started_ms_ = 0;
    lv_obj_t* message_label_ = nullptr;
    lv_obj_t* prompt_label_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
    std::unique_ptr<Tab5NativeApps> apps_;
    std::function<void()> radio_voice_action_;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* clock_digits_[6] = {};
    lv_obj_t* clock_flaps_[6] = {};
    lv_obj_t* clock_flap_digits_[6] = {};
    char clock_digit_values_[6] = {'-', '-', '-', '-', '-', '-'};
    time_t last_clock_second_ = 0;
    bool home_clock_hidden_ = false;
    int last_date_key_ = -1;
    lv_obj_t* waveform_[5] = {};
    lv_timer_t* animation_timer_ = nullptr;
    uint32_t animation_period_ms_ = 40;
    unsigned tick_ = 0;
    uint32_t animation_started_ms_ = 0;
    uint32_t frame_stats_started_ms_ = 0;
    int64_t render_started_us_ = 0;
    tab5_frame::CauseGate frame_cause_gate_;
    tab5_frame::Summary frame_cause_summary_;
    uint8_t frame_causes_ = 0;
    uint8_t frame_pose_entry_action_ = tab5_frame::kNoPoseAction;
    uint64_t frame_flush_pixels_ = 0;
    uint64_t render_total_us_ = 0;
    uint64_t pose_render_total_us_ = 0;
    uint64_t flushed_pixels_ = 0;
    uint64_t pose_flushed_pixels_ = 0;
    uint64_t tick_total_us_ = 0;
    uint32_t render_max_us_ = 0;
    uint32_t pose_render_max_us_ = 0;
    uint32_t other_render_max_us_ = 0;
    uint32_t tick_max_us_ = 0;
    uint32_t render_frames_ = 0;
    uint32_t pose_render_frames_ = 0;
    uint32_t pose_throttle_count_ = 0;
    uint32_t idle_major_updates_ = 0;
    uint32_t tick_count_ = 0;
    uint32_t slow_render_frames_ = 0;
    uint32_t pose_slow_render_frames_ = 0;
    uint32_t slow_ticks_ = 0;
    uint32_t flush_count_ = 0;
    uint32_t last_status_dot_phase_ = UINT32_MAX;
    uint32_t last_sleep_tick_ = UINT32_MAX;
    bool wave_active_ = false;
    bool pose_congested_ = false;
    bool greeting_active_ = false;
    bool active_ = false;
    bool speaking_ = false;
    bool sleeping_ = false;
    bool preview_active_ = false;
    std::unique_ptr<LvglAllocatedImage> vision_preview_image_;
    uint16_t* vision_preview_pixels_ = nullptr;
    std::function<void()> presence_test_action_;
    std::function<void()> interaction_action_;
    unsigned mouth_frame_ = 0;
    lv_opa_t mouth_opa_ = LV_OPA_COVER;
    lv_opa_t ambient_opa_[3] = {};
    lv_opa_t thought_opa_[3] = {};
    std::string music_title_;
    std::string music_artist_;
    tab5_lrc::Document music_lyrics_;
    int music_line_index_ = -2;
    int64_t music_elapsed_ms_ = 0;
    int64_t music_resume_ms_ = 0;
    uint32_t music_stream_open_sequence_ = 0;
    bool music_playing_ = false;
    bool music_active_ = false;
    bool music_seen_station_ = false;
    tab5_radio_status::LatestMailbox radio_status_mailbox_;
    // Tick runs on the LVGL task, whose remaining stack is tight. Keep its
    // 932-byte consumer snapshot in the display object rather than on that stack.
    tab5_radio_status::Snapshot radio_status_current_;

    void RefreshMusicLyrics() {
        if (music_lyrics_.lines.empty())
            return;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const int64_t elapsed =
            music_elapsed_ms_ + (music_playing_ ? now_ms - music_resume_ms_ : 0);
        const int index = tab5_lrc::ActiveLine(
            music_lyrics_, static_cast<uint32_t>(std::max<int64_t>(0, elapsed)));
        if (index == music_line_index_)
            return;
        music_line_index_ = index;
        const auto& lines = music_lyrics_.lines;
        const char* previous = index > 0 ? lines[index - 1].text.c_str() : "";
        const char* current = index >= 0 ? lines[index].text.c_str() : "♪";
        const char* next =
            index + 1 < static_cast<int>(lines.size()) ? lines[index + 1].text.c_str() : "";
        if (message_label_)
            lv_label_set_text(message_label_, current);
        if (apps_)
            apps_->SetMusicLyricsWindow(music_title_.c_str(), music_artist_.c_str(), previous,
                                        current, next);
    }

    // Called only while LVGL owns the display context. The radio decoder posts
    // status to a mailbox so it never waits for a slow render frame here.
    void UpdateMusicPlaybackStateLocked(const char* station, const char* state,
                                        uint32_t stream_open_sequence = 0) {
        if (!music_active_)
            return;
        const std::string expected =
            music_title_ + (music_artist_.empty() ? "" : " - " + music_artist_);
        if (station && *station && station == expected)
            music_seen_station_ = true;
        if (music_seen_station_ && station && *station && station != expected) {
            music_active_ = false;
            music_playing_ = false;
            music_lyrics_ = {};
            if (apps_)
                apps_->ClearMusicLyrics();
            return;
        }
        if (!music_seen_station_)
            return;
        // A broken HTTP stream is reopened from byte zero, not resumed with a
        // Range request. Rewind the lyrics even if Connecting was overwritten
        // by Buffering/Playing in the latest-state mailbox before this tick.
        if (stream_open_sequence != 0 && stream_open_sequence != music_stream_open_sequence_) {
            music_stream_open_sequence_ = stream_open_sequence;
            music_elapsed_ms_ = 0;
            music_resume_ms_ = 0;
            music_playing_ = false;
            music_line_index_ = -2;
            RefreshMusicLyrics();
        }
        const bool playing = state && std::strcmp(state, "Playing") == 0;
        if (playing == music_playing_)
            return;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (playing)
            music_resume_ms_ = now_ms;
        else
            music_elapsed_ms_ += now_ms - music_resume_ms_;
        music_playing_ = playing;
        if (playing)
            next_idle_reaction_ms_ = static_cast<uint64_t>(now_ms);
    }

#ifdef CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT
    static void ExperimentalFlush(lv_display_t* display, const lv_area_t* area,
                                  uint8_t* color_map) {
        auto* self = static_cast<QdtechTab5Display*>(lv_display_get_user_data(display));
        if (!self || !area || !color_map || !self->rotation_output_) {
            lv_display_flush_ready(display);
            return;
        }
        self->FlushRotatedArea(display, area, color_map);
    }

    void ReleaseVerificationBuffer() {
        if (verify_output_)
            heap_caps_free(verify_output_);
        verify_output_ = nullptr;
    }

    void FlushRotatedArea(lv_display_t* display, const lv_area_t* area, uint8_t* color_map) {
        const int32_t width = lv_area_get_width(area);
        const int32_t height = lv_area_get_height(area);
        if (width <= 0 || height <= 0) {
            ++flush_errors_;
            lv_display_flush_ready(display);
            return;
        }
        const size_t pixels = static_cast<size_t>(width) * height;
        const uint32_t source_stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
        const size_t source_bytes = static_cast<size_t>(source_stride) * height;
        const size_t draw_buffer_bytes = static_cast<size_t>(width_) * 50 * sizeof(uint16_t);
        if (pixels > static_cast<size_t>(width_) * 50 ||
            source_stride < static_cast<uint32_t>(width * sizeof(uint16_t)) ||
            source_bytes > draw_buffer_bytes ||
            pixels * sizeof(uint16_t) > rotation_output_bytes_) {
            ++flush_errors_;
            lv_display_flush_ready(display);
            return;
        }
        const int32_t logical_height = lv_display_get_vertical_resolution(display);
        const int32_t x0 = logical_height - area->y2 - 1;
        const int32_t y0 = area->x1;
        const int32_t x1 = x0 + height;
        const int32_t y1 = y0 + width;
        if (x0 < 0 || y0 < 0 || x1 > width_ || y1 > height_) {
            ++flush_errors_;
            lv_display_flush_ready(display);
            return;
        }

        bool rotation_ready = false;
        if (ppa_client_ && !ppa_disabled_ && pixels >= kPpaMinimumPixels &&
            source_stride % sizeof(uint16_t) == 0) {
            ppa_srm_oper_config_t config = {};
            config.in.buffer = color_map;
            config.in.pic_w = source_stride / sizeof(uint16_t);
            config.in.pic_h = height;
            config.in.block_w = width;
            config.in.block_h = height;
            config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            config.out.buffer = rotation_output_;
            config.out.buffer_size = rotation_output_bytes_;
            config.out.pic_w = height;
            config.out.pic_h = width;
            config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            config.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
            config.scale_x = 1.0f;
            config.scale_y = 1.0f;
            config.mode = PPA_TRANS_MODE_BLOCKING;
            // The existing Tab5 DSI path uses unswapped RGB565. Both rotation
            // paths leave the byte order unchanged.
            config.byte_swap = false;

            const int64_t started_us = esp_timer_get_time();
            const esp_err_t err = ppa_do_scale_rotate_mirror(ppa_client_, &config);
            const uint32_t ppa_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
            ++ppa_attempts_;
            ppa_total_us_ += ppa_us;
            ppa_max_us_ = std::max(ppa_max_us_, ppa_us);
            if (err == ESP_OK) {
                rotation_ready = true;
                ppa_consecutive_failures_ = 0;
                if (verify_output_) {
                    // Compare only the compact, valid RGB565 pixels. The
                    // output allocation may have cache-line padding.
                    lv_draw_sw_rotate(color_map, verify_output_, width, height, source_stride,
                                      static_cast<int32_t>(height * sizeof(uint16_t)),
                                      LV_DISPLAY_ROTATION_270, LV_COLOR_FORMAT_RGB565);
                    const size_t valid_bytes = pixels * sizeof(uint16_t);
                    if (std::memcmp(rotation_output_, verify_output_, valid_bytes) != 0) {
                        ++verification_mismatches_;
                        ppa_disabled_ = true;
                        // Preserve the verified software pixels for this flush.
                        std::memcpy(rotation_output_, verify_output_, valid_bytes);
                        ++sw_rotations_;
                        ReleaseVerificationBuffer();
                        ESP_LOGE("Tab5Ppa",
                                 "PPA RGB565 mismatch for %ldx%ld; software fallback active",
                                 static_cast<long>(width), static_cast<long>(height));
                    } else if (++verified_regions_ == kPpaVerifyRegions) {
                        ReleaseVerificationBuffer();
                        ESP_LOGI("Tab5Ppa", "PPA RGB565 verified on %u dirty regions",
                                 static_cast<unsigned>(kPpaVerifyRegions));
                    }
                }
                if (!ppa_disabled_)
                    ++ppa_successes_;
            } else {
                ++ppa_failures_;
                if (++ppa_consecutive_failures_ >= 3) {
                    ppa_disabled_ = true;
                    ReleaseVerificationBuffer();
                    ESP_LOGW("Tab5Ppa",
                             "PPA rotation disabled after 3 errors; software fallback active");
                }
            }
        }

        if (!rotation_ready) {
            lv_draw_sw_rotate(color_map, rotation_output_, width, height, source_stride,
                              static_cast<int32_t>(height * sizeof(uint16_t)),
                              LV_DISPLAY_ROTATION_270, LV_COLOR_FORMAT_RGB565);
            ++sw_rotations_;
        }

        // DSI partial mode sends flush_ready from on_color_trans_done. Do not
        // also call it here unless draw_bitmap rejected the transfer.
        if (esp_lcd_panel_draw_bitmap(panel_, x0, y0, x1, y1, rotation_output_) != ESP_OK) {
            ++flush_errors_;
            lv_display_flush_ready(display);
        }
    }

    void InitExperimentalFlush() {
        if (!display_ || lv_display_get_color_format(display_) != LV_COLOR_FORMAT_RGB565 ||
            lv_display_get_rotation(display_) != LV_DISPLAY_ROTATION_270) {
            ESP_LOGW("Tab5Ppa", "PPA experiment skipped: unexpected display format or rotation");
            return;
        }

        constexpr size_t kAlignment = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
        const size_t draw_bytes = static_cast<size_t>(width_) * 50 * sizeof(uint16_t);
        rotation_output_bytes_ = (draw_bytes + kAlignment - 1) & ~(kAlignment - 1);
        rotation_output_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            kAlignment, rotation_output_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rotation_output_) {
            ESP_LOGW("Tab5Ppa", "PPA experiment skipped: no rotation buffer");
            rotation_output_bytes_ = 0;
            return;
        }
        verify_output_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            kAlignment, rotation_output_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!verify_output_) {
            ESP_LOGW("Tab5Ppa", "PPA experiment skipped: no verification buffer");
            heap_caps_free(rotation_output_);
            rotation_output_ = nullptr;
            rotation_output_bytes_ = 0;
            return;
        }

        ppa_client_config_t config = {};
        config.oper_type = PPA_OPERATION_SRM;
        const esp_err_t err = ppa_register_client(&config, &ppa_client_);
        if (err != ESP_OK) {
            ESP_LOGW("Tab5Ppa", "PPA experiment skipped: client registration failed (%s)",
                     esp_err_to_name(err));
            ReleaseVerificationBuffer();
            heap_caps_free(rotation_output_);
            rotation_output_ = nullptr;
            rotation_output_bytes_ = 0;
            return;
        }

        lv_display_set_user_data(display_, this);
        lv_display_set_flush_cb(display_, ExperimentalFlush);
        ESP_LOGI("Tab5Ppa",
                 "Experimental PPA rotation active; threshold=%u px, PSRAM=%u+%u B until %u checks",
                 static_cast<unsigned>(kPpaMinimumPixels),
                 static_cast<unsigned>(rotation_output_bytes_),
                 static_cast<unsigned>(rotation_output_bytes_),
                 static_cast<unsigned>(kPpaVerifyRegions));
    }
#endif

    static void ClockFlapScale(void* obj, int32_t value) {
        lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(obj), value, 0);
    }

    static void SetLabelTextIfChanged(lv_obj_t* label, const char* text) {
        if (!label)
            return;
        const char* next = text ? text : "";
        const char* current = lv_label_get_text(label);
        if (!current || std::strcmp(current, next) != 0)
            lv_label_set_text(label, next);
    }

    // Measure work actually submitted to LVGL. Render-ready includes drawing
    // and flush callback time, but may not include the asynchronous panel DMA.
    static void FrameProfileEvent(lv_event_t* event) {
        auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
        switch (lv_event_get_code(event)) {
            case LV_EVENT_RENDER_START:
                self->render_started_us_ = esp_timer_get_time();
                self->frame_causes_ = self->frame_cause_gate_.BeginFrame(lv_tick_get());
                self->frame_pose_entry_action_ = self->frame_cause_gate_.PoseEntryAction();
                self->frame_flush_pixels_ = 0;
                break;
            case LV_EVENT_RENDER_READY:
                if (self->render_started_us_) {
                    const uint32_t elapsed =
                        static_cast<uint32_t>(esp_timer_get_time() - self->render_started_us_);
                    self->render_total_us_ += elapsed;
                    self->render_max_us_ = std::max(self->render_max_us_, elapsed);
                    ++self->render_frames_;
                    if (self->wave_active_) {
                        self->pose_render_total_us_ += elapsed;
                        self->pose_render_max_us_ = std::max(self->pose_render_max_us_, elapsed);
                        ++self->pose_render_frames_;
                        self->pose_slow_render_frames_ += elapsed > 40000;
                    } else {
                        self->other_render_max_us_ = std::max(self->other_render_max_us_, elapsed);
                    }
                    self->slow_render_frames_ += elapsed > 40000;
                    self->frame_cause_summary_.Observe(self->frame_causes_, elapsed,
                                                       self->frame_flush_pixels_,
                                                       self->frame_pose_entry_action_);
                    // A large transformed pose occasionally needs longer
                    // than one display period. Leave breathing room for AFE
                    // for the rest of this short action, without changing
                    // mouth/clock/app refresh rates.
                    if (self->wave_active_ && elapsed > 35000 && !self->pose_congested_) {
                        self->pose_congested_ = true;
                        ++self->pose_throttle_count_;
                    }
                    self->render_started_us_ = 0;
                }
                self->OnDailyRenderReady();
                break;
            case LV_EVENT_FLUSH_START: {
                const auto* area = static_cast<const lv_area_t*>(lv_event_get_param(event));
                if (area) {
                    const uint64_t pixels =
                        static_cast<uint64_t>(lv_area_get_width(area)) * lv_area_get_height(area);
                    self->flushed_pixels_ += pixels;
                    self->frame_flush_pixels_ += pixels;
                    if (self->wave_active_)
                        self->pose_flushed_pixels_ += pixels;
                    ++self->flush_count_;
                }
                break;
            }
            default:
                break;
        }
    }

    void RecordTickTime(int64_t started_us) {
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started_us);
        tick_total_us_ += elapsed;
        tick_max_us_ = std::max(tick_max_us_, elapsed);
        ++tick_count_;
        slow_ticks_ += elapsed > 20000;
    }

    void ReportFrameStats(uint32_t now) {
        const uint32_t elapsed_ms = now - frame_stats_started_ms_;
        if (elapsed_ms < 10000)
            return;
        const uint32_t average_us =
            render_frames_ ? static_cast<uint32_t>(render_total_us_ / render_frames_) : 0;
        const uint32_t pose_average_us =
            pose_render_frames_ ? static_cast<uint32_t>(pose_render_total_us_ / pose_render_frames_)
                                : 0;
        const uint32_t average_tick_us =
            tick_count_ ? static_cast<uint32_t>(tick_total_us_ / tick_count_) : 0;
        const char* scene = preview_active_               ? "preview"
                            : sleeping_                   ? "sleep"
                            : apps_ && apps_->IsVisible() ? "app"
                            : wave_active_                ? "pose"
                            : speaking_                   ? "speech"
                                                          : "home";
        ESP_LOGI("Tab5Frame",
                 "%u ms last=%s: render=%u avg=%u.%03u max=%u.%03u ms slow40=%u "
                 "pose=%u/%llu px avg=%u.%03u max=%u.%03u slow40=%u other_max=%u.%03u "
                 "idle_keys=%u throttle=%u flush=%u total=%llu px tick=%u avg=%u.%03u "
                 "max=%u.%03u ms "
                 "slow20=%u",
                 elapsed_ms, scene, render_frames_, average_us / 1000, average_us % 1000,
                 render_max_us_ / 1000, render_max_us_ % 1000, slow_render_frames_,
                 pose_render_frames_, static_cast<unsigned long long>(pose_flushed_pixels_),
                 pose_average_us / 1000, pose_average_us % 1000, pose_render_max_us_ / 1000,
                 pose_render_max_us_ % 1000, pose_slow_render_frames_, other_render_max_us_ / 1000,
                 other_render_max_us_ % 1000, idle_major_updates_, pose_throttle_count_,
                 flush_count_, static_cast<unsigned long long>(flushed_pixels_), tick_count_,
                 average_tick_us / 1000, average_tick_us % 1000, tick_max_us_ / 1000,
                 tick_max_us_ % 1000, slow_ticks_);
        const auto& exit = frame_cause_summary_.Get(0);
        const auto& sleep = frame_cause_summary_.Get(1);
        const auto& daily = frame_cause_summary_.Get(2);
        const auto& clock = frame_cause_summary_.Get(3);
        const auto& entry = frame_cause_summary_.Get(tab5_frame::kPoseEntryIndex);
        const auto& unmarked = frame_cause_summary_.Get(tab5_frame::kUnmarked);
        // Each category reports frames/slow40/max_us/pixels_at_max. A frame
        // with multiple marks is counted in every matching category.
        ESP_LOGI("Tab5Cause",
                 "%u ms frames/slow40/max_us/px pose_exit=%u/%u/%u/%llu "
                 "sleep_switch=%u/%u/%u/%llu daily_page=%u/%u/%u/%llu "
                 "clock_flip=%u/%u/%u/%llu pose_entry=%u/%u/%u/%llu entry_action=%u "
                 "unmarked=%u/%u/%u/%llu multi40=%u",
                 elapsed_ms, exit.frames, exit.slow40, exit.max_us,
                 static_cast<unsigned long long>(exit.pixels_at_max), sleep.frames, sleep.slow40,
                 sleep.max_us, static_cast<unsigned long long>(sleep.pixels_at_max), daily.frames,
                 daily.slow40, daily.max_us, static_cast<unsigned long long>(daily.pixels_at_max),
                 clock.frames, clock.slow40, clock.max_us,
                 static_cast<unsigned long long>(clock.pixels_at_max), entry.frames, entry.slow40,
                 entry.max_us, static_cast<unsigned long long>(entry.pixels_at_max),
                 static_cast<unsigned>(frame_cause_summary_.PoseEntryActionAtMax()),
                 unmarked.frames, unmarked.slow40, unmarked.max_us,
                 static_cast<unsigned long long>(unmarked.pixels_at_max),
                 frame_cause_summary_.multi_slow40());
        frame_cause_summary_.Reset();
#ifdef CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT
        if (rotation_output_) {
            const uint32_t ppa_average_us =
                ppa_attempts_ ? static_cast<uint32_t>(ppa_total_us_ / ppa_attempts_) : 0;
            ESP_LOGI("Tab5Ppa",
                     "flush ppa=%u/%u failed=%u sw=%u disabled=%u avg=%u.%03u max=%u.%03u ms "
                     "verify=%u/%u mismatches=%u verifier=%u draw_errors=%u",
                     ppa_successes_, ppa_attempts_, ppa_failures_, sw_rotations_,
                     static_cast<unsigned>(ppa_disabled_), ppa_average_us / 1000,
                     ppa_average_us % 1000, ppa_max_us_ / 1000, ppa_max_us_ % 1000,
                     static_cast<unsigned>(verified_regions_),
                     static_cast<unsigned>(kPpaVerifyRegions), verification_mismatches_,
                     static_cast<unsigned>(verify_output_ != nullptr), flush_errors_);
        }
        ppa_attempts_ = ppa_successes_ = ppa_failures_ = sw_rotations_ = flush_errors_ =
            ppa_max_us_ = 0;
        ppa_total_us_ = 0;
#endif
        frame_stats_started_ms_ = now;
        render_total_us_ = pose_render_total_us_ = flushed_pixels_ = pose_flushed_pixels_ =
            tick_total_us_ = 0;
        render_max_us_ = pose_render_max_us_ = other_render_max_us_ = tick_max_us_ =
            render_frames_ = pose_render_frames_ = idle_major_updates_ = pose_throttle_count_ =
                tick_count_ = slow_render_frames_ = pose_slow_render_frames_ = slow_ticks_ =
                    flush_count_ = 0;
    }

    void UpdateAnimationTimerPeriod() {
        const uint32_t period = wave_active_ ? (pose_congested_ ? 50 : 40)
                                : (sleeping_ || preview_active_) && !(apps_ && apps_->IsVisible())
                                    ? 100
                                    : 40;
        if (animation_timer_ && period != animation_period_ms_) {
            animation_period_ms_ = period;
            lv_timer_set_period(animation_timer_, period);
        }
    }

    // Pages 0-2 only exist once something was pushed; pages 6-8 need today's date.
    bool DailyPageValid(unsigned page) const {
        if (page < 3)
            return page < digest_count_;
        if (page >= 6)
            return daily_date_key_ != -1;
        return true;
    }

    unsigned NextDailyPage() const {
        unsigned next = daily_page_;
        for (unsigned i = 0; i < kDailyPages; ++i) {
            next = (next + 1) % kDailyPages;
            if (DailyPageValid(next))
                return next;
        }
        return daily_page_;
    }

    void ShowDailyPage(unsigned page) {
        const unsigned next_page = page % kDailyPages;
        const bool page_changed = next_page != daily_page_;
        daily_page_ = next_page;
        ++daily_page_revision_;
        // Clinical pearls need longer to read than the one-line cards.
        next_page_tick_ = tick_ + (daily_page_ >= 6 ? 300 : 160);
        if (!daily_title_label_ || !daily_body_label_)
            return;
        // Give each carousel topic its own quiet accent. This changes only on
        // page turns, so the card gains hierarchy without a recurring redraw.
        static constexpr uint32_t kAccent[kDailyPages] = {
            0x76d8ed, 0x76d8ed, 0x76d8ed,  // Pushed medical digest
            0xf7c84d, 0xb3a5ef, 0xffa581,  // Quote, history, festival
            0x91d7af, 0x91d7af, 0x91d7af,  // Clinical pearls
        };
        const lv_color_t accent = lv_color_hex(kAccent[daily_page_]);
        if (daily_accent_)
            lv_obj_set_style_bg_color(daily_accent_, accent, 0);
        lv_obj_set_style_text_color(daily_title_label_, accent, 0);
        lv_label_set_text(daily_title_label_, daily_titles_[daily_page_].c_str());
        lv_label_set_text(daily_body_label_, daily_bodies_[daily_page_].c_str());
        for (unsigned i = 0; i < kDailyPages; ++i) {
            SetVisible(daily_dots_[i], DailyPageValid(i));
            lv_obj_set_style_bg_opa(daily_dots_[i], i == daily_page_ ? LV_OPA_COVER : LV_OPA_30, 0);
        }
        const bool pearl = daily_page_ >= 6 && daily_date_key_ != -1;
        if ((has_digest_ && daily_page_ < 3) || pearl) {
            digest_prompt_ = pearl ? "临床干货" : "今日医学精选";
            digest_text_ = daily_titles_[daily_page_] + "\n" + daily_bodies_[daily_page_];
            if (pearl && !daily_sources_[daily_page_].empty())
                digest_text_ += "\n来源：" + daily_sources_[daily_page_];
            const bool can_mirror =
                !active_ && !speaking_ && !music_active_ && prompt_label_ && message_label_;
            const bool apps_visible = apps_ && apps_->IsVisible();
            if (can_mirror && !apps_visible) {
                daily_mirror_retry_on_home_ = false;
                daily_mirror_gate_.Queue(daily_page_, daily_page_revision_,
                                         lv_label_get_text(prompt_label_),
                                         lv_label_get_text(message_label_));
            } else {
                daily_mirror_retry_on_home_ = can_mirror && apps_visible;
                daily_mirror_gate_.Cancel();
            }
        } else {
            daily_mirror_retry_on_home_ = false;
            daily_mirror_gate_.Cancel();
        }
        if (page_changed)
            frame_cause_gate_.MarkOnce(tab5_frame::kDailyPage, lv_tick_get());
    }

    void ApplyDailyMirrorAfterCardRender() {
        if (!daily_mirror_gate_.HasPending())
            return;
        if (apps_ && apps_->IsVisible()) {
            daily_mirror_retry_on_home_ = true;
            daily_mirror_gate_.Cancel();
            return;
        }
        const bool idle = !active_ && !speaking_ && !music_active_;
        if (!daily_mirror_gate_.Take(daily_page_, daily_page_revision_, idle,
                                     lv_label_get_text(prompt_label_),
                                     lv_label_get_text(message_label_)))
            return;
        const bool changed =
            std::strcmp(lv_label_get_text(prompt_label_), digest_prompt_.c_str()) != 0 ||
            std::strcmp(lv_label_get_text(message_label_), digest_text_.c_str()) != 0;
        SetLabelTextIfChanged(prompt_label_, digest_prompt_.c_str());
        SetLabelTextIfChanged(message_label_, digest_text_.c_str());
        if (changed)
            frame_cause_gate_.MarkOnce(tab5_frame::kDailyPage, lv_tick_get());
    }

    void OnDailyRenderReady() {
        if (apps_ && apps_->IsVisible()) {
            if (daily_mirror_gate_.HasPending())
                daily_mirror_retry_on_home_ = true;
            daily_mirror_gate_.Cancel();
            return;
        }
        daily_mirror_gate_.RenderReady();
    }

    // Fill builtin trio immediately so the carousel is never empty before SNTP.
    void InitDailyFallback() {
        daily_titles_[3] = "每日一句";
        daily_bodies_[3] = "正在校时…";
        daily_titles_[4] = "历史上的今天";
        daily_bodies_[4] = "正在校时…";
        daily_titles_[5] = "节日提醒";
        daily_bodies_[5] = "正在校时…";
        for (int i = 6; i < 9; ++i) {
            daily_titles_[i] = "临床干货";
            daily_bodies_[i] = "正在校时…";
        }
        if (!has_digest_) {
            daily_titles_[0] = "今日医学";
            daily_bodies_[0] = "等待推送…";
            daily_titles_[1] = "今日医学";
            daily_bodies_[1] = "等待推送…";
            daily_titles_[2] = "今日医学";
            daily_bodies_[2] = "等待推送…";
        }
        ShowDailyPage(has_digest_ ? 0 : 3);
    }

    void UpdateDailyContent(const tm& date) {
        const int key = (date.tm_year + 1900) * 10000 + (date.tm_mon + 1) * 100 + date.tm_mday;
        if (key == daily_date_key_)
            return;
        const bool clock_was_unset = daily_date_key_ == -1;
        if (daily_date_key_ != -1) {
            has_digest_ = false;
            digest_count_ = 0;
            digest_text_.clear();
            if (!active_ && !speaking_ && !music_active_) {
                if (prompt_label_)
                    lv_label_set_text(prompt_label_, "随时倾听");
                if (message_label_)
                    lv_label_set_text(message_label_, "轻触下方按钮，开始对话。");
            }
        }
        daily_date_key_ = key;
        if (!has_digest_)
            LoadPersistedDailyCards(key);
        else if (clock_was_unset)
            QueueDailyPersist();
        const auto content = Tab5DailyContentForDate(date);
        // Pages 3-5 keep the original daily trio.
        daily_titles_[3] = "每日一句";
        daily_bodies_[3] = content.quote;
        daily_titles_[4] = "历史上的今天";
        daily_bodies_[4] = content.history_text
                               ? std::string(content.history_year) + " · " + content.history_text
                               : "今天暂无收录的历史事件";
        daily_titles_[5] = "节日提醒";
        if (content.festival_title) {
            daily_bodies_[5] =
                std::string("今天是") + content.festival_title + " · " + content.festival_text;
        } else if (content.next_festival_title) {
            daily_bodies_[5] = std::string(content.next_festival_title) + "还有" +
                               std::to_string(content.days_to_next_festival) + "天";
        } else {
            daily_bodies_[5] = "愿今天也有值得记住的小事";
        }
        const auto pearls = Tab5PearlsForDate(date);
        for (int i = 0; i < 3; ++i) {
            daily_titles_[6 + i] = std::string(pearls.prefix[i]) + pearls.item[i].title;
            daily_bodies_[6 + i] = pearls.item[i].body;
            daily_sources_[6 + i] = pearls.item[i].source;
        }
        if (!has_digest_) {
            daily_titles_[0] = "今日医学";
            daily_bodies_[0] = "等待推送…";
            daily_titles_[1] = "今日医学";
            daily_bodies_[1] = "等待推送…";
            daily_titles_[2] = "今日医学";
            daily_bodies_[2] = "等待推送…";
        }
        ShowDailyPage(has_digest_ ? 0 : 3);
    }

    static void PresenceLongPress(lv_event_t* event) {
        auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
        if (self->presence_test_action_)
            self->presence_test_action_();
    }

    static bool IsRadioRequest(const char* role, const char* content) {
        if (!role || std::strcmp(role, "user") != 0 || !content)
            return false;
        const std::string_view text(content);
        for (const char* negative : {"不要", "不想", "停止", "关闭", "别播", "别放"})
            if (text.find(negative) != std::string_view::npos)
                return false;
        for (const char* phrase : {"听广播", "播放广播", "放广播", "听电台", "打开电台"})
            if (text.find(phrase) != std::string_view::npos)
                return true;
        return false;
    }

    void UpdateClock(bool app_visible = false) {
        const bool returning_home = home_clock_hidden_ && !app_visible;
        if (home_clock_hidden_ != app_visible) {
            home_clock_hidden_ = app_visible;
            if (app_visible) {
                // The app page covers the clock, but LVGL animations still run
                // beneath it unless they are explicitly stopped.
                for (auto* flap : clock_flaps_) {
                    lv_anim_delete(flap, ClockFlapScale);
                    lv_obj_add_flag(flap, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_transform_scale_y(flap, 256, 0);
                }
            }
        }
        const time_t now = time(nullptr);
        if (now == last_clock_second_ && !returning_home)
            return;
        last_clock_second_ = now;
        tm local{};
        if (!localtime_r(&now, &local) || local.tm_year < 125)
            return;

        if (!app_visible) {
            bool started_flip = false;
            char digits[7];
            std::snprintf(digits, sizeof(digits), "%02d%02d%02d", local.tm_hour, local.tm_min,
                          local.tm_sec);
            for (int i = 0; i < 6; ++i) {
                if (clock_digit_values_[i] == digits[i])
                    continue;
                const char old = clock_digit_values_[i];
                clock_digit_values_[i] = digits[i];
                char text[2] = {digits[i], 0};
                lv_label_set_text(clock_digits_[i], text);
                if (returning_home || old < '0' || old > '9')
                    continue;
                char previous[2] = {old, 0};
                lv_label_set_text(clock_flap_digits_[i], previous);
                lv_anim_delete(clock_flaps_[i], ClockFlapScale);
                lv_obj_set_style_transform_scale_y(clock_flaps_[i], 256, 0);
                lv_obj_remove_flag(clock_flaps_[i], LV_OBJ_FLAG_HIDDEN);
                lv_anim_t flip;
                lv_anim_init(&flip);
                lv_anim_set_var(&flip, clock_flaps_[i]);
                lv_anim_set_values(&flip, 256, 0);
                lv_anim_set_duration(&flip, 180);
                lv_anim_set_path_cb(&flip, lv_anim_path_ease_in);
                lv_anim_set_exec_cb(&flip, ClockFlapScale);
                lv_anim_set_completed_cb(&flip, [](lv_anim_t* animation) {
                    auto* flap = static_cast<lv_obj_t*>(animation->var);
                    lv_obj_add_flag(flap, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_transform_scale_y(flap, 256, 0);
                });
                lv_anim_start(&flip);
                started_flip = true;
            }
            if (started_flip)
                frame_cause_gate_.StartClockFlip(lv_tick_get());
        }
        const int date_key = local.tm_year * 366 + local.tm_yday;
        if (date_key != last_date_key_) {
            static constexpr const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
            char date[48];
            std::snprintf(date, sizeof(date), "%04d.%02d.%02d  周%s", local.tm_year + 1900,
                          local.tm_mon + 1, local.tm_mday, weekdays[local.tm_wday]);
            lv_label_set_text(date_label_, date);
            last_date_key_ = date_key;
        }
        UpdateDailyContent(local);
    }

    static lv_obj_t* Label(lv_obj_t* parent, const char* value, const lv_font_t* font,
                           uint32_t color, int x, int y, int width) {
        auto* label = lv_label_create(parent);
        lv_label_set_text(label, value);
        lv_obj_set_pos(label, x, y);
        // long_mode first: setting WRAP after width resets the object to
        // LV_SIZE_CONTENT and the line gets clipped by the parent card.
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, width);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        return label;
    }

    static lv_obj_t* Card(lv_obj_t* parent, int x, int y, int w, int h, uint32_t fill,
                          uint32_t border, int radius) {
        auto* obj = lv_obj_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_radius(obj, radius, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    }

    static void SetVisible(lv_obj_t* obj, bool visible) {
        if (!obj || visible == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
            return;
        if (visible)
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    static lv_obj_t* WavePart(lv_obj_t* parent, const lv_image_dsc_t* source,
                              const nabo::LayerGeometry& geometry) {
        auto* part = lv_image_create(parent);
        lv_image_set_src(part, source);
        lv_obj_set_pos(part, geometry.x, geometry.y);
        lv_image_set_pivot(part, geometry.pivot_x, geometry.pivot_y);
        lv_image_set_antialias(part, true);
        lv_obj_clear_flag(part, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
        return part;
    }

    void DropMotionCache() {
        for (auto* part : {wave_legs_, wave_torso_, wave_head_, wave_hand_, wave_cuff_}) {
            if (part)
                lv_image_cache_drop(lv_image_get_src(part));
        }
    }

    void ConfigureMotionRig(nabo::Action action) {
        DropMotionCache();
        major_pose_valid_ = false;
        for (auto& part : displayed_pose_)
            part.valid = false;
        auto bind = [](lv_obj_t* image, const lv_image_dsc_t* source,
                       const nabo::LayerGeometry& geometry) {
            lv_image_set_src(image, source);
            lv_image_set_pivot(image, geometry.pivot_x, geometry.pivot_y);
        };
        pose_has_hand_ = action == nabo::Action::Wave;
        if (action == nabo::Action::Wave) {
            pose_has_head_ = true;
            pose_geometry_ = NaboWaveGeometry();
            bind(wave_legs_, &nabo_wave_legs, pose_geometry_.legs);
            bind(wave_torso_, &nabo_wave_torso, pose_geometry_.torso);
            bind(wave_head_, &nabo_wave_head, pose_geometry_.head);
            bind(wave_hand_, &nabo_wave_hand, pose_geometry_.hand);
            bind(wave_cuff_, &nabo_wave_cuff, pose_geometry_.cuff);
        } else {
            const auto rig = NaboReactionRig(action);
            pose_geometry_ = rig.geometry;
            pose_has_head_ = rig.head != nullptr;
            pose_has_hand_ = rig.hand != nullptr && rig.cuff != nullptr;
            bind(wave_legs_, rig.legs, pose_geometry_.legs);
            const lv_image_dsc_t* torso = rig.torso;
#ifdef CONFIG_QDTECH_TAB5_RAW_THINK_TORSO_EXPERIMENT
            if (action == nabo::Action::Think) {
                if (nabo_think_torso_raw_available())
                    torso = &nabo_think_torso_raw;
                else
                    ESP_LOGE("NaboPose", "Raw Think torso invalid; using compressed image");
            }
#endif
            bind(wave_torso_, torso, pose_geometry_.torso);
            if (pose_has_head_)
                bind(wave_head_, rig.head, pose_geometry_.head);
            if (pose_has_hand_) {
                bind(wave_hand_, rig.hand, pose_geometry_.hand);
                bind(wave_cuff_, rig.cuff, pose_geometry_.cuff);
            }
        }
    }

    bool MotionRigReady(nabo::Action action) {
        // Pin the complete pose while validating, before hiding the portrait.
        std::unique_ptr<lv_image_decoder_dsc_t[]> checks(new (std::nothrow)
                                                             lv_image_decoder_dsc_t[5]{});
        if (!checks)
            return false;
        unsigned opened = 0;
        bool ready = true;
        for (auto* part : {wave_legs_, wave_torso_, wave_head_, wave_hand_, wave_cuff_}) {
            if ((part == wave_head_ && !pose_has_head_) ||
                ((part == wave_hand_ || part == wave_cuff_) && !pose_has_hand_))
                continue;
            const void* source = lv_image_get_src(part);
            if (!source ||
                lv_image_decoder_open(&checks[opened], source, nullptr) != LV_RESULT_OK) {
                ESP_LOGW("NaboPose", "Pose %d layer %u decode failed; keeping complete portrait",
                         static_cast<int>(action), opened);
                ready = false;
                break;
            }
            ++opened;
        }
        for (unsigned i = 0; i < opened; ++i)
            lv_image_decoder_close(&checks[i]);
        return ready;
    }

    void ApplyWaveMotion(const nabo::WaveMotion& motion, uint64_t now_ms) {
        auto place = [this](unsigned index, lv_obj_t* image, const nabo::LayerPose& part) -> bool {
            auto& shown = displayed_pose_[index];
            const int x = nabo::Pixel(part.x);
            const int y = nabo::Pixel(part.y);
            const int angle = (part.angle + 3600) % 3600;
            const bool x_changed = !shown.valid || shown.x != x;
            const bool y_changed = !shown.valid || shown.y != y;
            const bool angle_changed = !shown.valid || shown.angle != angle;
            if (x_changed)
                lv_obj_set_x(image, x);
            if (y_changed)
                lv_obj_set_y(image, y);
            if (angle_changed)
                lv_image_set_rotation(image, angle);
            shown = {x, y, angle, true};
            return x_changed || y_changed || angle_changed;
        };

        bool update_major = !idle_pose_ || !major_pose_valid_;
        if (idle_pose_) {
            const uint64_t age = now_ms - pose_started_ms_;
            const unsigned key_count = sizeof(kIdleMajorKeysMs) / sizeof(kIdleMajorKeysMs[0]);
            // A delayed LVGL frame may cross several keys at once. Use only
            // the latest pose and allow an audio window after each large
            // redraw instead of immediately queuing another one.
            if (next_idle_major_key_ < key_count && age >= kIdleMajorKeysMs[next_idle_major_key_] &&
                (!major_pose_valid_ || now_ms - last_major_pose_ms_ >= 250)) {
                update_major = true;
                do {
                    ++next_idle_major_key_;
                } while (next_idle_major_key_ < key_count &&
                         age >= kIdleMajorKeysMs[next_idle_major_key_]);
            }
        }
        if (update_major) {
            auto artwork_motion = motion;
            if (idle_pose_) {
                // Avoid rotating the large pose images during automatic idle actions.
                artwork_motion.torso_angle = 0;
                artwork_motion.head_angle = 0;
                if (!pose_has_head_) {
                    // A 1–2 pixel bob would redraw the entire merged head/body image.
                    artwork_motion.body_x = 0;
                    artwork_motion.body_y = 0;
                }
            }
            major_pose_ = nabo::BuildWavePose(pose_geometry_, artwork_motion);
            major_motion_ = artwork_motion;
            major_pose_valid_ = true;
            last_major_pose_ms_ = now_ms;
            bool changed = place(0, wave_legs_, major_pose_.legs);
            changed |= place(1, wave_torso_, major_pose_.torso);
            if (pose_has_head_)
                changed |= place(2, wave_head_, major_pose_.head);
            idle_major_updates_ += idle_pose_ && changed;
        }
        if (pose_has_hand_) {
            // Hands stay pinned to the last body key pose while their small
            // local rotation can run at the normal animation cadence.
            auto hand = major_pose_.hand;
            hand.angle += motion.hand_angle - major_motion_.hand_angle;
            place(3, wave_hand_, hand);
            place(4, wave_cuff_, major_pose_.cuff);
        }
    }

    void SetWaveRig(bool enabled) {
        for (auto* part : {wave_legs_, wave_torso_, wave_head_, wave_hand_, wave_cuff_}) {
            if (!part)
                continue;
            if (enabled && (pose_has_head_ || part != wave_head_) &&
                (pose_has_hand_ || (part != wave_hand_ && part != wave_cuff_)))
                lv_obj_remove_flag(part, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
        }
        if (!enabled) {
            pose_congested_ = false;
            idle_pose_ = false;
            major_pose_valid_ = false;
            DropMotionCache();
        }
        // Keep decoded PNGs for the current motion and release them when it ends.
        UpdateAnimationTimerPeriod();
        if (auto* refresh = lv_display_get_refr_timer(display_))
            lv_timer_set_period(refresh, LV_DEF_REFR_PERIOD);
    }

    void CancelWave() {
        const bool had_pose = wave_active_;
        wave_active_ = false;
        pose_animation_.Stop();
        SetWaveRig(false);
        SetVisible(wave_, false);
        SetVisible(portrait_, !sleeping_ && !preview_active_);
        if (had_pose)
            frame_cause_gate_.MarkOnce(tab5_frame::kPoseExit, lv_tick_get());
    }

    static uint64_t AnimationTimeMs() { return esp_timer_get_time() / 1000; }

    void ShowAction(nabo::Action action, bool automatic_idle = false) {
        // Keep live speaking mouth patches visible; large pose swaps are brief.
        if (speaking_ || preview_active_ || (active_ && action == nabo::Action::Wave))
            return;
        const uint64_t now = AnimationTimeMs();
        if (pose_animation_.Current(now) == action)
            return;
        if (sleeping_) {
            sleeping_ = false;
            lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
            for (auto* z : sleep_z_)
                if (z)
                    lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
            for (auto* dot : ambient_)
                if (dot)
                    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            frame_cause_gate_.MarkOnce(tab5_frame::kSleepSwitch, lv_tick_get());
        }
        if (nabo::Animation::HasMotion(action)) {
            ConfigureMotionRig(action);
            if (!MotionRigReady(action)) {
                CancelWave();
                return;
            }
        }
        wave_active_ = true;
        pose_congested_ = false;
        idle_pose_ = automatic_idle;
        pose_started_ms_ = now;
        next_idle_major_key_ = 0;
        pose_animation_.Start(action, now);
        face_animation_.ResetEyes(lv_tick_get());
        eye_frame_ = 0;
        lv_obj_set_x(wave_, 106);
        lv_obj_add_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        mouth_frame_ = 0;
        lv_obj_remove_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        if (nabo::Animation::HasMotion(action))
            ApplyWaveMotion(pose_animation_.Motion(now), now);
        SetWaveRig(nabo::Animation::HasMotion(action));
        frame_cause_gate_.MarkPoseEntry(static_cast<uint8_t>(action), lv_tick_get());
    }

    void ShowWave() {
        if (interaction_action_)
            interaction_action_();
        ShowAction(nabo::Action::Wave);
    }

    void TouchNabo() {
        if (interaction_action_)
            interaction_action_();
        pending_emotion_.Clear();
        const uint64_t now = AnimationTimeMs();
        const bool double_tap = last_touch_ms_ && now - last_touch_ms_ < 400;
        last_touch_ms_ = now;
        static constexpr nabo::Action reactions[] = {
            nabo::Action::Wave, nabo::Action::Wink, nabo::Action::Encourage, nabo::Action::Curious};
        ShowAction(double_tap ? nabo::Action::Happy : reactions[touch_reaction_++ % 4]);
        next_idle_reaction_ms_ = now + 18000;
    }

    void Tick() {
        const uint32_t now = lv_tick_get();
        UpdateAnimationTimerPeriod();
        ReportFrameStats(now);
        const int64_t tick_started_us = esp_timer_get_time();
        const unsigned service_tick = (now - animation_started_ms_) / 50;
        // Keep application polling/carousel cadence at 50 ms while face
        // patches target 25 fps. Heavy pose frames may slow to 20 fps.
        // A delayed callback never runs a catch-up loop.
        const bool service_due = service_tick != tick_;
        if (service_due) {
            const bool refresh_wifi = service_tick / 40 != tick_ / 40;
            tick_ = service_tick;
            if (radio_status_mailbox_.Take(radio_status_current_)) {
                UpdateMusicPlaybackStateLocked(radio_status_current_.station.data(),
                                               radio_status_current_.state.data(),
                                               radio_status_current_.stream_open_sequence);
                if (apps_)
                    apps_->SetRadioState(radio_status_current_.station.data(),
                                         radio_status_current_.state.data(),
                                         radio_status_current_.meta.data());
            }
            if (apps_)
                apps_->Tick();
            if (apps_ && apps_->IsSettingsVisible() && refresh_wifi)
                apps_->RefreshWifi();
            RefreshMusicLyrics();
        }
        const bool apps_visible = apps_ && apps_->IsVisible();
        const bool returning_home = home_clock_hidden_ && !apps_visible;
        if (service_due || apps_visible != home_clock_hidden_)
            UpdateClock(apps_visible);
        if (apps_visible) {
            if (daily_mirror_gate_.HasPending())
                daily_mirror_retry_on_home_ = true;
            daily_mirror_gate_.Cancel();
            pending_emotion_.Clear();
            if (wave_active_)
                CancelWave();
            RecordTickTime(tick_started_us);
            return;
        }
        if (returning_home || daily_mirror_retry_on_home_)
            ShowDailyPage(daily_page_);
        if (tick_ >= next_page_tick_)
            ShowDailyPage(NextDailyPage());
        ApplyDailyMirrorAfterCardRender();
        if (preview_active_) {
            RecordTickTime(tick_started_us);
            return;
        }
        if (sleeping_) {
            const uint32_t sleep_tick = now / 100;
            if (sleep_tick != last_sleep_tick_) {
                last_sleep_tick_ = sleep_tick;
                for (unsigned i = 0; i < 3; ++i) {
                    const uint32_t phase = (now + i * 1600) % 4800;
                    lv_obj_set_y(sleep_z_[i], 135 - static_cast<int>(phase * 48 / 4800));
                    const int fade = phase < 2400 ? phase : 4800 - phase;
                    lv_obj_set_style_opa(sleep_z_[i], 35 + fade * 196 / 2400, 0);
                }
            }
            RecordTickTime(tick_started_us);
            return;
        }
        if (active_ && status_dot_) {
            const uint32_t phase = now / 180;
            if (phase != last_status_dot_phase_) {
                last_status_dot_phase_ = phase;
                static constexpr lv_opa_t kPulse[] = {LV_OPA_70, LV_OPA_COVER, LV_OPA_COVER,
                                                      LV_OPA_70};
                lv_obj_set_style_opa(status_dot_, kPulse[phase % 4], 0);
            }
        }
        if (greeting_active_ && now - greeting_started_ms_ >= 3000)
            greeting_active_ = false;
        const uint64_t pose_now = AnimationTimeMs();
        const auto current_pose = pose_animation_.Current(pose_now);
        if (active_ && !speaking_ && Application::GetInstance().IsVoiceDetected()) {
            pending_emotion_.Clear();
            if (current_pose != nabo::Action::Idle && current_pose != nabo::Action::Listen &&
                current_pose != nabo::Action::Think)
                ShowAction(nabo::Action::Listen);
        }
        const bool speech_pending =
            speaking_ || !Application::GetInstance().GetAudioService().IsPlaybackIdle();
        const auto pending =
            pending_emotion_.Take(pose_now,
                                  !speech_pending && !awaiting_reply_ &&
                                      pose_animation_.Current(pose_now) == nabo::Action::Idle &&
                                      (!active_ || !Application::GetInstance().IsVoiceDetected()),
                                  speech_pending);
        if (pending != nabo::Action::Idle) {
            ShowAction(pending);
            next_idle_reaction_ms_ = pose_now + 18000;
        }
        if (!active_ && !wave_active_ && pose_now >= next_idle_reaction_ms_) {
            ShowAction(music_playing_ ? nabo::Action::Music : nabo::IdleAction(idle_reaction_++),
                       true);
            next_idle_reaction_ms_ = pose_now + 18000 + pose_now % 7000;
        }
        if (wave_active_) {
            const auto action = pose_animation_.Current(pose_now);
            const int frame = pose_animation_.Frame(pose_now);
            if (frame < 0) {
                CancelWave();
                face_animation_.ResetEyes(now);
            } else if (nabo::Animation::HasMotion(action)) {
                ApplyWaveMotion(pose_animation_.Motion(pose_now), pose_now);
            }
        }
        auto& application = Application::GetInstance();
        const bool playback = speaking_ && !application.GetAudioService().IsPlaybackIdle();
        auto frame =
            face_animation_.Sample(now, active_ && !speaking_ && application.IsVoiceDetected(),
                                   playback, greeting_active_);
        if (idle_pose_) {
            const uint32_t duration =
                static_cast<uint32_t>(nabo::Animation::Duration(pose_animation_.Current(pose_now)));
            const uint32_t age =
                static_cast<uint32_t>(std::min<uint64_t>(pose_now - pose_started_ms_, duration));
            const uint32_t fade = std::min<uint32_t>({age, duration - age, 240}) * 255 / 240;
            const unsigned phase = (age / 160) % 6;
            static constexpr uint8_t kGlints[] = {0, 70, 125, 170, 125, 70};
            for (unsigned i = 0; i < 3; ++i)
                frame.thought_opa[i] = kGlints[(phase + i * 2) % 6] * fade / 255;
        }
        // Blink and mouth stay aligned with the original portrait. No full-body
        // translation/scale or per-frame allocations in the speaking path.
#ifdef CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT
        const unsigned eyes = (wave_active_ || !portrait_matte_valid_) ? 0 : frame.eyes;
#else
        const unsigned eyes = wave_active_ ? 0 : frame.eyes;
#endif
        if (eyes != eye_frame_ || (eyes && lv_obj_has_flag(blink_, LV_OBJ_FLAG_HIDDEN))) {
            eye_frame_ = eyes;
            if (!eyes) {
                lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_image_set_src(blink_, eyes == 1 ? &nabo_half_blink : &nabo_closed_blink);
                lv_obj_remove_flag(blink_, LV_OBJ_FLAG_HIDDEN);
            }
        }
#ifdef CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT
        const unsigned mouth = (wave_active_ || !portrait_matte_valid_) ? 0 : frame.mouth;
#else
        const unsigned mouth = wave_active_ ? 0 : frame.mouth;
#endif
        if (mouth != mouth_frame_ || (mouth && lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN))) {
            mouth_frame_ = mouth;
            if (!mouth) {
                lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_image_set_src(mouth_, mouth == 1 ? &nabo_mouth_half : &nabo_mouth_open);
                lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (mouth && frame.mouth_opa != mouth_opa_) {
            mouth_opa_ = frame.mouth_opa;
            lv_obj_set_style_opa(mouth_, mouth_opa_, 0);
        }
        for (unsigned i = 0; i < 3; ++i) {
            lv_obj_set_y(ambient_[i], frame.ambient_y[i]);
            if (ambient_opa_[i] != frame.ambient_opa[i]) {
                ambient_opa_[i] = frame.ambient_opa[i];
                lv_obj_set_style_opa(ambient_[i], ambient_opa_[i], 0);
            }
            SetVisible(thought_[i], frame.thought_opa[i] != 0);
            if (frame.thought_opa[i] && thought_opa_[i] != frame.thought_opa[i]) {
                thought_opa_[i] = frame.thought_opa[i];
                lv_obj_set_style_opa(thought_[i], thought_opa_[i], 0);
            }
        }
        for (unsigned i = 0; i < 5; ++i) {
            if (bar_heights_[i] == frame.bars[i])
                continue;
            bar_heights_[i] = frame.bars[i];
            lv_obj_set_height(waveform_[i], frame.bars[i]);
            lv_obj_set_y(waveform_[i], (48 - frame.bars[i]) / 2);
        }
        RecordTickTime(tick_started_us);
    }

public:
    QdtechTab5Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                      bool swap_xy)
        : MipiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                         swap_xy) {
        DisplayLockGuard lock(this);
        lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);
#ifdef CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT
        InitExperimentalFlush();
#endif
    }

    ~QdtechTab5Display() override {
        DisplayLockGuard lock(this);
        if (animation_timer_) {
            lv_timer_delete(animation_timer_);
            animation_timer_ = nullptr;
        }
        if (lock.locked())
            lv_display_remove_event_cb_with_user_data(display_, FrameProfileEvent, this);
        if (vision_preview_image_) {
            if (lock.locked() && preview_image_) {
                lv_image_set_src(preview_image_, nullptr);
                lv_image_cache_drop(vision_preview_image_->image_dsc());
            }
            vision_preview_image_.reset();
            vision_preview_pixels_ = nullptr;
        }
    }

    void SetupUI() override {
        if (setup_ui_called_)
            return;
        Display::SetupUI();
        DisplayLockGuard lock(this);
        const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        // Keep the current pose decoded while leaving PSRAM for AFE and person detection.
        const size_t art_cache = std::min<size_t>(1024 * 1024, psram_free / 2);
        lv_image_cache_resize(art_cache, false);
        auto* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0d1b2b), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        Card(screen, 32, 32, 8, 53, 0x62c8e9, 0x62c8e9, 4);
        Label(screen, "NABO", &qd_font_lxgw_36, 0xf7fbff, 57, 36, 210);
        Label(screen, "土皮助手", &qd_font_lxgw_28, 0x96afc5, 193, 43, 200);

        auto* clock = lv_obj_create(screen);
        lv_obj_set_pos(clock, 405, 11);
        lv_obj_set_size(clock, 520, 90);
        lv_obj_set_style_bg_opa(clock, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(clock, 0, 0);
        lv_obj_set_style_pad_all(clock, 0, 0);
        lv_obj_clear_flag(clock, LV_OBJ_FLAG_SCROLLABLE);
        static constexpr int digit_x[6] = {0, 82, 180, 262, 360, 442};
        for (int i = 0; i < 6; ++i) {
            constexpr int width = 78;
            constexpr int height = 86;
            constexpr int half = height / 2;
            constexpr int numeral_y = 5;
            const uint32_t numeral_color = i < 2 ? 0xf8faf8 : i < 4 ? 0xf7c84d : 0x9acddb;
            auto* tile = Card(clock, digit_x[i], 2, width, height, 0x13293d, 0x31536c, 9);
            Card(tile, 1, 1, width - 2, half - 1, 0x1c3448, 0x1c3448, 8);
            clock_digits_[i] =
                Label(tile, "-", &qd_font_clock_72, numeral_color, 0, numeral_y, width);
            lv_obj_set_style_text_align(clock_digits_[i], LV_TEXT_ALIGN_CENTER, 0);
            auto* flap = Card(tile, 1, 1, width - 2, half, 0x1c3448, 0x1c3448, 8);
            lv_obj_set_style_transform_pivot_y(flap, half, 0);
            clock_flap_digits_[i] =
                Label(flap, "-", &qd_font_clock_72, numeral_color, -1, numeral_y - 1, width);
            lv_obj_set_style_text_align(clock_flap_digits_[i], LV_TEXT_ALIGN_CENTER, 0);
            clock_flaps_[i] = flap;
            lv_obj_add_flag(flap, LV_OBJ_FLAG_HIDDEN);
            Card(tile, 1, half, width - 2, 1, 0x4b6578, 0x4b6578, 0);
        }
        for (int x : {166, 346}) {
            Card(clock, x, 29, 7, 7, 0xa4d5e2, 0xa4d5e2, 4);
            Card(clock, x, 58, 7, 7, 0xa4d5e2, 0xa4d5e2, 4);
        }

        date_label_ = Label(screen, "等待校时", &qd_font_lxgw_28, 0xe4edf5, 965, 27, 280);
        lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_RIGHT, 0);
        auto* status_pill = Card(screen, 965, 61, 283, 39, 0x19354a, 0x315d72, 19);
        lv_obj_set_style_border_width(status_pill, 0, 0);
        status_dot_ = Card(status_pill, 19, 14, 11, 11, 0x79d7b4, 0x79d7b4, 6);
        lv_obj_set_style_border_width(status_dot_, 0, 0);
        status_label_ = Label(status_pill, "已就绪", &qd_font_lxgw_28, 0xb5dce9, 43, 1, 221);
        lv_obj_set_height(status_label_, 35);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
        // LvglDisplay swaps this label with status_label_ while a notification
        // is active. Keep both in the same pill so daily cards stay readable.
        notification_label_ = Label(status_pill, "", &qd_font_lxgw_28, 0xffd493, 43, 1, 221);
        lv_obj_set_height(notification_label_, 35);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        emoji_box_ = Card(screen, 36, 112, 512, 558, 0x112842, 0x285272, 34);
        Card(emoji_box_, 42, 28, 422, 492, 0x183550, 0x183550, 220);
        portrait_ = lv_image_create(emoji_box_);
#ifdef CONFIG_QDTECH_TAB5_PORTRAIT_MATTE_EXPERIMENT
        portrait_matte_valid_ = nabo_portrait_matte_available();
        if (portrait_matte_valid_) {
            lv_image_set_src(portrait_, &nabo_portrait_matte);
            lv_obj_set_pos(portrait_, 39, 0);  // 434x558 visible region, no scaling.
        } else {
            ESP_LOGE("Tab5Portrait", "Precomposed portrait invalid; using built-in sleep art");
            lv_image_set_src(portrait_, &nabo_sleep);
            lv_obj_set_pos(portrait_, 15, 167);
        }
#else
        lv_image_set_src(portrait_, &nabo_portrait);
        lv_obj_set_pos(portrait_, 39, 0);  // Original 434x618 pixels, no scaling.
#endif
        lv_obj_add_flag(portrait_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            portrait_,
            [](lv_event_t* e) {
                static_cast<QdtechTab5Display*>(lv_event_get_user_data(e))->TouchNabo();
            },
            LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(portrait_, PresenceLongPress, LV_EVENT_LONG_PRESSED, this);

        blink_ = lv_image_create(emoji_box_);
        lv_image_set_src(blink_, &nabo_half_blink);
        lv_obj_set_pos(blink_, 104, 246);  // Matches source sheet crop at x65,y310.
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);

        mouth_ = lv_image_create(emoji_box_);
        lv_image_set_src(mouth_, &nabo_mouth_half);
        lv_obj_set_pos(mouth_, 231, 361);  // Matches portrait mouth at source x216,y441.
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

        wave_ = lv_obj_create(emoji_box_);
        lv_obj_remove_style_all(wave_);
        lv_obj_set_size(wave_, 300, 561);
        lv_obj_clear_flag(wave_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(wave_, 106, -4);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            wave_,
            [](lv_event_t* e) {
                static_cast<QdtechTab5Display*>(lv_event_get_user_data(e))->TouchNabo();
            },
            LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(wave_, PresenceLongPress, LV_EVENT_LONG_PRESSED, this);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        constexpr auto rig = NaboWaveGeometry();
        wave_legs_ = WavePart(wave_, &nabo_wave_legs, rig.legs);
        wave_torso_ = WavePart(wave_, &nabo_wave_torso, rig.torso);
        wave_hand_ = WavePart(wave_, &nabo_wave_hand, rig.hand);
        wave_cuff_ = WavePart(wave_, &nabo_wave_cuff, rig.cuff);
        wave_head_ = WavePart(wave_, &nabo_wave_head, rig.head);

        sleep_ = lv_image_create(emoji_box_);
        lv_image_set_src(sleep_, &nabo_sleep);
        lv_obj_set_pos(sleep_, 15, 167);
        lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            sleep_,
            [](lv_event_t* e) {
                static_cast<QdtechTab5Display*>(lv_event_get_user_data(e))->TouchNabo();
            },
            LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(sleep_, PresenceLongPress, LV_EVENT_LONG_PRESSED, this);

        for (int i = 0; i < 3; ++i) {
            sleep_z_[i] = Label(emoji_box_, "z", &qd_font_lxgw_28, 0xa7d9ec, 344 + i * 32, 135, 28);
            lv_obj_add_flag(sleep_z_[i], LV_OBJ_FLAG_HIDDEN);
        }
        constexpr int ambient_x[3] = {67, 432, 76};
        for (int i = 0; i < 3; ++i) {
            ambient_[i] = Card(emoji_box_, ambient_x[i], 112 + i * 69, 7 + i * 2, 7 + i * 2,
                               0x81d8eb, 0x81d8eb, 12);
            lv_obj_set_style_opa(ambient_[i], LV_OPA_30, 0);
        }

        for (unsigned i = 0; i < 3; ++i) {
            thought_[i] = Card(emoji_box_, 414 + i * 17, 72, 8, 8, 0xa7d9ec, 0xa7d9ec, 4);
            lv_obj_add_flag(thought_[i], LV_OBJ_FLAG_HIDDEN);
        }

        auto* daily_card = Card(screen, 581, 112, 647, 143, 0x122b43, 0x2c536c, 26);
        lv_obj_add_flag(daily_card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            daily_card,
            [](lv_event_t* event) {
                auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
                self->ShowDailyPage(self->NextDailyPage());
            },
            LV_EVENT_CLICKED, this);
        daily_accent_ = Card(daily_card, 24, 23, 5, 93, 0x58c9e7, 0x58c9e7, 2);
        lv_obj_set_style_border_width(daily_accent_, 0, 0);
        lv_obj_add_flag(daily_accent_, LV_OBJ_FLAG_EVENT_BUBBLE);
        daily_title_label_ = Label(daily_card, "每日一句", &qd_font_cjk_28, 0x76d8ed, 46, 12, 430);
        daily_body_label_ = Label(daily_card, "等待校时", &qd_font_cjk_28, 0xf5f9fd, 46, 52, 555);
        lv_obj_add_flag(daily_title_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(daily_body_label_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_height(daily_body_label_, 82);
        for (unsigned i = 0; i < kDailyPages; ++i) {
            daily_dots_[i] = Card(daily_card, 500 + i * 16, 23, 8, 8, 0x76d8ed, 0x76d8ed, 5);
            lv_obj_add_flag(daily_dots_[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        InitDailyFallback();

        auto* card = Card(screen, 581, 264, 647, 245, 0x122b43, 0x2c536c, 28);
        Card(card, 24, 28, 5, 43, 0x58c9e7, 0x58c9e7, 2);
        prompt_label_ = Label(card, "随时倾听", &qd_font_lxgw_28, 0x76d8ed, 46, 26, 360);
        message_label_ =
            Label(card, "轻触下方按钮，开始对话。", &qd_font_cjk_28, 0xe1eff6, 46, 91, 552);
        lv_obj_set_height(message_label_, 140);

        auto* apps_button = Card(screen, 1005, 279, 100, 53, 0x193b56, 0x3a7490, 18);
        lv_obj_add_flag(apps_button, LV_OBJ_FLAG_CLICKABLE);
        auto* apps_label = Label(apps_button, "应用", &qd_font_lxgw_28, 0xb6e8f4, 14, 10, 72);
        lv_obj_set_style_text_align(apps_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_event_cb(
            apps_button,
            [](lv_event_t* event) {
                auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
                if (self->apps_)
                    self->apps_->OpenApps();
            },
            LV_EVENT_CLICKED, this);
        auto* settings_button = Card(screen, 1114, 279, 100, 53, 0x193b56, 0x3a7490, 18);
        lv_obj_add_flag(settings_button, LV_OBJ_FLAG_CLICKABLE);
        auto* settings_label =
            Label(settings_button, "设置", &qd_font_lxgw_28, 0xb6e8f4, 14, 10, 72);
        lv_obj_set_style_text_align(settings_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_event_cb(
            settings_button,
            [](lv_event_t* event) {
                auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
                if (self->apps_)
                    self->apps_->OpenSettings();
            },
            LV_EVENT_CLICKED, this);
        auto* button = lv_btn_create(screen);
        lv_obj_set_pos(button, 581, 536);
        lv_obj_set_size(button, 647, 108);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x69d0e8), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x99e4f1), LV_STATE_PRESSED);
        lv_obj_set_style_radius(button, 30, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(
            button,
            [](lv_event_t* e) {
                auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(e));
                ESP_LOGI("Tab5Native", "Chat button clicked");
                self->ShowWave();
                lv_label_set_text(
                    self->status_label_,
                    self->speaking_ ? "正在倾听" : (self->active_ ? "正在结束" : "正在启动"));
                Application::GetInstance().Schedule(
                    [] { Application::GetInstance().ToggleChatState(); });
            },
            LV_EVENT_CLICKED, this);
        button_label_ = lv_label_create(button);
        lv_label_set_text(button_label_, "开始对话");
        lv_obj_set_style_text_font(button_label_, &qd_font_lxgw_36, 0);
        lv_obj_set_style_text_color(button_label_, lv_color_hex(0x0b2b3f), 0);
        lv_obj_center(button_label_);

        auto* meter = Card(screen, 1040, 655, 159, 52, 0x1b3a52, 0x1b3a52, 25);
        for (int i = 0; i < 5; ++i)
            waveform_[i] = Card(meter, 32 + i * 22, 17, 7, 18, 0x69d0e8, 0x69d0e8, 4);

        preview_image_ = lv_image_create(screen);
        lv_obj_set_size(preview_image_, 320, 240);
        lv_obj_set_pos(preview_image_, 132, 266);
        lv_image_set_scale(preview_image_, 384);  // 1.5x, fits Nabo's left card.
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

        apps_ = std::make_unique<Tab5NativeApps>(screen);

        animation_started_ms_ = lv_tick_get();
        next_idle_reaction_ms_ = AnimationTimeMs() + 18000;
        animation_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<QdtechTab5Display*>(lv_timer_get_user_data(timer))->Tick();
            },
            40, this);
        frame_stats_started_ms_ = animation_started_ms_;
        lv_display_add_event_cb(display_, FrameProfileEvent, LV_EVENT_RENDER_START, this);
        lv_display_add_event_cb(display_, FrameProfileEvent, LV_EVENT_RENDER_READY, this);
        lv_display_add_event_cb(display_, FrameProfileEvent, LV_EVENT_FLUSH_START, this);
        UpdateClock();
    }

    void SetStatus(const char* status) override {
        DisplayLockGuard lock(this);
        if (!lock.locked() || !status_label_)
            return;
        const bool is_clock = status && std::strlen(status) == 5 && status[0] >= '0' &&
                              status[0] <= '9' && status[1] >= '0' && status[1] <= '9' &&
                              status[2] == ':' && status[3] >= '0' && status[3] <= '9' &&
                              status[4] >= '0' && status[4] <= '9';
        if (is_clock)
            return;
        const bool connecting = status && std::strcmp(status, Lang::Strings::CONNECTING) == 0;
        SetLabelTextIfChanged(status_label_, status);
        const bool was_active = active_;
        active_ = connecting || (status && (std::strcmp(status, Lang::Strings::LISTENING) == 0 ||
                                            std::strcmp(status, Lang::Strings::SPEAKING) == 0));
        speaking_ = status && std::strcmp(status, Lang::Strings::SPEAKING) == 0;
        if (status_dot_) {
            const uint32_t color = speaking_    ? 0xf5c078
                                   : connecting ? 0xe7b86b
                                   : active_    ? 0x75d8f0
                                                : 0x79d7b4;
            if (lv_color_to_u32(lv_obj_get_style_bg_color(status_dot_, LV_PART_MAIN)) !=
                lv_color_to_u32(lv_color_hex(color)))
                lv_obj_set_style_bg_color(status_dot_, lv_color_hex(color), 0);
            if (!active_ && was_active) {
                last_status_dot_phase_ = UINT32_MAX;
                lv_obj_set_style_opa(status_dot_, LV_OPA_COVER, 0);
            }
        }
        if (!is_clock) {
            awaiting_reply_ = status && std::strcmp(status, Lang::Strings::CONNECTING) == 0;
            if (awaiting_reply_)
                pending_emotion_.Clear();
        }
        const auto previous_state = face_animation_.state();
        face_animation_.SetState(speaking_    ? tab5_home::State::Speaking
                                 : connecting ? tab5_home::State::Thinking
                                 : active_    ? tab5_home::State::Listening
                                              : tab5_home::State::Idle,
                                 lv_tick_get());
        if (active_ && previous_state != face_animation_.state()) {
            CancelWave();
            greeting_active_ = false;
            if (!speaking_ && !connecting && previous_state != tab5_home::State::Listening)
                ShowAction(nabo::Action::Listen);
        }
        if (button_label_)
            SetLabelTextIfChanged(button_label_,
                                  speaking_ ? "继续对话" : (active_ ? "结束对话" : "开始对话"));
        if (prompt_label_)
            SetLabelTextIfChanged(
                prompt_label_,
                speaking_
                    ? "Nabo 正在回应"
                    : (connecting
                           ? "正在连接 Nabo…"
                           : (active_ ? "Nabo 正在倾听"
                                      : (digest_text_.empty() ? "随时倾听"
                                                              : digest_prompt_.c_str()))));
        if (!active_ && !speaking_ && !digest_text_.empty() && message_label_)
            SetLabelTextIfChanged(message_label_, digest_text_.c_str());
        if (apps_) {
            if (speaking_)
                apps_->SetVoiceStatus("● Nabo 正在回应", true);
            else if (connecting)
                apps_->SetVoiceStatus("正在连接 Nabo…", true);
            else if (active_)
                apps_->SetVoiceStatus("● 正在聆听，请说歌名", true);
            else
                apps_->SetVoiceStatus("", false);
        }
    }

    void SetChatMessage(const char* role, const char* content) override {
        const bool play_radio = IsRadioRequest(role, content);
        const bool has = content && *content;
        // Idle digest is sticky: ignore empty/heartbeat chat frames so the
        // workbench brief stays on screen until a real conversation starts.
        if (!has && has_digest_ && !active_ && !speaking_)
            return;
        {
            DisplayLockGuard lock(this);
            if (!message_label_)
                return;
            lv_label_set_text(message_label_, has ? content : "轻触下方按钮，开始对话。");
            if (role && std::strcmp(role, "user") == 0 && has) {
                pending_emotion_.Clear();
                awaiting_reply_ = true;
            }
            if (role && std::strcmp(role, "user") == 0 && has && active_ && !speaking_) {
                face_animation_.SetState(tab5_home::State::Thinking, lv_tick_get());
                CancelWave();
                ShowAction(nabo::Action::Think);
                lv_label_set_text(prompt_label_, "Nabo 正在思考");
            }
            if (role && std::strcmp(role, "assistant") == 0 && has)
                lv_label_set_text(prompt_label_, speaking_ ? "Nabo 说" : "Nabo 正在思考");
            if (apps_ && has && (active_ || speaking_) && role) {
                const bool user = std::strcmp(role, "user") == 0;
                if (user || std::strcmp(role, "assistant") == 0) {
                    std::string line = user ? "你：" : "Nabo：";
                    line += content;
                    apps_->SetVoiceStatus(line.c_str(), true);
                }
            }
            if (play_radio && apps_)
                apps_->OpenRadio(!radio_voice_action_);
        }
        if (play_radio && radio_voice_action_)
            Application::GetInstance().Schedule([action = radio_voice_action_] { action(); });
    }

    void ClearChatMessages() override {
        DisplayLockGuard lock(this);
        ShowIdlePanel();
    }

    void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (!emotion || !portrait_ || preview_active_ || sleeping_ || (apps_ && apps_->IsVisible()))
            return;
        pending_emotion_.Store(nabo::EmotionAction(emotion), AnimationTimeMs());
    }

    void SetSleeping(bool sleeping) {
        DisplayLockGuard lock(this);
        if (!sleep_ || sleeping_ == sleeping)
            return;
        sleeping_ = sleeping;
        pending_emotion_.Clear();
        awaiting_reply_ = false;
        wave_active_ = false;
        pose_animation_.Stop();
        SetWaveRig(false);
        next_idle_reaction_ms_ = AnimationTimeMs() + 18000;
        greeting_active_ = false;
        mouth_frame_ = eye_frame_ = 0;
        face_animation_.ResetEyes(lv_tick_get());
        for (auto* dot : thought_)
            SetVisible(dot, false);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        for (auto* dot : ambient_)
            if (dot) {
                if (sleeping)
                    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            }
        for (auto* z : sleep_z_)
            if (z) {
                if (sleeping)
                    lv_obj_remove_flag(z, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
            }
        if (sleeping) {
            lv_obj_add_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
            if (prompt_label_)
                lv_label_set_text(prompt_label_, "Nabo 正在休息");
        } else {
            lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            if (prompt_label_)
                lv_label_set_text(prompt_label_, "Nabo 醒来了");
        }
        UpdateAnimationTimerPeriod();
        frame_cause_gate_.MarkOnce(tab5_frame::kSleepSwitch, lv_tick_get());
    }

    void WelcomeBack() {
        DisplayLockGuard lock(this);
        const bool was_sleeping = sleeping_;
        if (sleep_)
            lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
        sleeping_ = false;
        UpdateAnimationTimerPeriod();
        for (auto* z : sleep_z_)
            if (z)
                lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
        for (auto* dot : ambient_)
            if (dot)
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
        // Keep the pushed digest visible; only flash the greeting in the prompt.
        if (prompt_label_)
            lv_label_set_text(prompt_label_, "你回来了");
        if (was_sleeping)
            frame_cause_gate_.MarkOnce(tab5_frame::kSleepSwitch, lv_tick_get());
        if (message_label_) {
            if (digest_text_.empty())
                lv_label_set_text(message_label_, "见到你真开心。今天想聊些什么？");
            else
                lv_label_set_text(message_label_, digest_text_.c_str());
        }
        ShowWave();
        greeting_active_ = !active_;
        greeting_started_ms_ = lv_tick_get();
    }

    void SetPresenceTestAction(std::function<void()> action) {
        DisplayLockGuard lock(this);
        presence_test_action_ = std::move(action);
    }

    void SetAppsActions(Tab5NativeApps::Actions actions, std::function<void()> voice_play) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetActions(std::move(actions));
        radio_voice_action_ = std::move(voice_play);
    }

    // Muse inbox update from the poll task. new_arrival: a message newer than any seen before
    // arrived; show it on the idle home panel so it is noticed without opening the app.
    void SetMuseInbox(const tab5_muse::Snapshot& snapshot, bool new_arrival) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (apps_)
            apps_->SetMuseInbox(snapshot);
        if (!new_arrival || active_ || speaking_ || snapshot.messages.empty())
            return;
        const auto& latest = snapshot.messages.front();
        if (prompt_label_)
            lv_label_set_text(prompt_label_, "Muse 新推送");
        if (message_label_) {
            std::string text = latest.title + "\n" + latest.body;
            lv_label_set_text(message_label_, text.c_str());
        }
    }

    void ShowRadioPage() {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->OpenRadio();
    }

    void SetFirmwareStatus(const std::string& text, const std::string& button, int progress,
                           bool busy) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetFirmwareStatus(text.c_str(), button.c_str(), progress, busy);
    }

    // Load a prebuilt LVGL .bin CJK font from SD and use it as fallback for
    // the flash-subset LXGW fonts (missing glyphs render blank otherwise).
    // Build: lv_font_conv --format bin --size 28 --bpp 4 --font <ttf> --symbols <chars> -o
    // cjk28.bin
    bool LoadSdFallbackFont() {
        if (sd_fallback_font_)
            return true;
        static const char* kCandidates[] = {
            "S:/fonts/cjk28.bin",
            "/sdcard/fonts/cjk28.bin",
        };
        for (const char* cand : kCandidates) {
            FILE* f = fopen(cand, "rb");
            if (!f)
                continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            // Reject empty / absurdly large files before handing to LVGL.
            if (sz < 1024 || sz > 2 * 1024 * 1024) {
                ESP_LOGW("Tab5Font", "Skip %s size=%ld", cand, sz);
                continue;
            }
            auto* font = lv_binfont_create(cand);
            if (!font) {
                ESP_LOGW("Tab5Font", "lv_binfont_create failed for %s", cand);
                continue;
            }
            sd_fallback_font_ = font;
            DisplayLockGuard lock(this);
            const_cast<lv_font_t*>(&qd_font_lxgw_28)->fallback = sd_fallback_font_;
            const_cast<lv_font_t*>(&qd_font_lxgw_36)->fallback = sd_fallback_font_;
            ESP_LOGI("Tab5Font", "SD fallback font ready: %s", cand);
            return true;
        }
        ESP_LOGW("Tab5Font", "No usable /sdcard/fonts/cjk28.bin; missing glyphs stay blank");
        return false;
    }

    void ShowIcuPage(int mode = -1, const std::string& result = "", bool result_ok = true) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->OpenIcu(mode, result, result_ok);
    }

    esp_lcd_panel_handle_t lcd_panel() const { return panel_; }
    lv_display_t* lv_disp() const { return display_; }

    void ShowNesPage() {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->OpenNes();
    }

    void PushNesFrame(const uint16_t* pixels, uint16_t width, uint16_t height) {
        // Scale outside the LVGL lock; only the invalidate needs the lock.
        // Holding the lock through a 1280×720 scale starves the display task.
        if (apps_) {
            apps_->SetNesFrame(pixels, width, height);
        }
    }

    void SetNesStatus(const char* status) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetNesStatus(status);
    }

    void SetNesPlaying(bool playing) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetNesPlaying(playing);
    }

    void SetRadioState(const char* station, const char* state, const char* meta) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (apps_)
            apps_->SetRadioState(station, state, meta);
    }

    void PostRadioState(const char* station, const char* state, const char* meta) {
        radio_status_mailbox_.Post(station, state, meta);
    }

    void SetInteractionAction(std::function<void()> action) {
        DisplayLockGuard lock(this);
        interaction_action_ = std::move(action);
    }

    void SetVisionMessage(const char* title, const char* message) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (prompt_label_)
            lv_label_set_text(prompt_label_, title);
        if (message_label_)
            lv_label_set_text(message_label_, message);
    }

    // Idle panel text: pushed digest when present, otherwise the default hint.
    void ShowIdlePanel() {
        if (prompt_label_)
            lv_label_set_text(prompt_label_, digest_text_.empty() ? "随时倾听"
                                                                  : digest_prompt_.c_str());
        if (message_label_)
            lv_label_set_text(message_label_, digest_text_.empty() ? "轻触下方按钮，开始对话。"
                                                                   : digest_text_.c_str());
    }

    // Push external digest onto the small daily card (stable all day) and
    // mirror it in the conversation panel when idle.
    void SetDailyCards(const char* const titles[3], const char* const bodies[3]) {
        {
            DisplayLockGuard lock(this);
            if (!lock.locked())
                return;
            unsigned count = 0;
            for (int i = 0; i < 3; ++i) {
                const char* title = titles[i];
                const char* body = bodies[i];
                if (!title || !*title)
                    continue;
                daily_titles_[count] = title;
                daily_bodies_[count] = body ? body : "";
                ++count;
            }
            for (unsigned i = count; i < 3; ++i) {
                daily_titles_[i].clear();
                daily_bodies_[i].clear();
            }
            digest_count_ = count;
            has_digest_ = count != 0;
            ShowDailyPage(has_digest_ ? 0 : 3);
        }
        QueueDailyPersist();
    }

    // OpenClaw pushes are kept in NVS together with the date they were pushed for,
    // so a reboot (or a crash) during the day no longer throws the digest away.
    static void PersistDailyCards(const DailyPersistSnapshot& snapshot) {
        if (snapshot.date_key <= 0)
            return;  // clock not set yet: cannot tell which day the cards belong to
        Settings settings("tab5daily", true);
        settings.SetInt("date", snapshot.date_key);
        for (int i = 0; i < 3; ++i) {
            settings.SetString("t" + std::to_string(i), snapshot.titles[i]);
            settings.SetString("b" + std::to_string(i), snapshot.bodies[i]);
        }
    }

    void QueueDailyPersist() {
        if (daily_persist_queued_.exchange(true, std::memory_order_acq_rel))
            return;
        Application::GetInstance().Schedule([this] {
            DailyPersistSnapshot snapshot;
            {
                DisplayLockGuard lock(this);
                if (!lock.locked()) {
                    daily_persist_queued_.store(false, std::memory_order_release);
                    return;
                }
                snapshot.date_key = daily_date_key_;
                for (int i = 0; i < 3; ++i) {
                    snapshot.titles[i] = daily_titles_[i];
                    snapshot.bodies[i] = daily_bodies_[i];
                }
                // A newer update after this point schedules another write.
                daily_persist_queued_.store(false, std::memory_order_release);
            }
            PersistDailyCards(snapshot);  // Application main task, without the display lock.
        });
    }

    // Called once the clock is known; restores today's cards if they were saved today.
    void LoadPersistedDailyCards(int today_key) {
        Settings settings("tab5daily", false);
        if (settings.GetInt("date", 0) != today_key)
            return;
        unsigned count = 0;
        for (int i = 0; i < 3; ++i) {
            auto title = settings.GetString("t" + std::to_string(i));
            auto body = settings.GetString("b" + std::to_string(i));
            // Older versions saved the empty-slot placeholder as a real card.
            if (title.empty() || (title == "今日医学" && body == "等待推送…"))
                continue;
            daily_titles_[count] = std::move(title);
            daily_bodies_[count] = std::move(body);
            ++count;
        }
        for (unsigned i = count; i < 3; ++i) {
            daily_titles_[i].clear();
            daily_bodies_[i].clear();
        }
        digest_count_ = count;
        has_digest_ = count != 0;
        if (has_digest_) {
            ESP_LOGI("Tab5Daily", "restored today's digest from NVS (%d)", today_key);
        }
    }

    void RestoreDigestIfIdle() {
        if (digest_text_.empty() || active_ || speaking_)
            return;
        if (prompt_label_)
            lv_label_set_text(prompt_label_, digest_prompt_.c_str());
        if (message_label_)
            lv_label_set_text(message_label_, digest_text_.c_str());
    }

    void SetMusicInfo(const char* title, const char* artist, const char* line) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (auto* theme = LvglThemeManager::GetInstance().GetTheme("dark")) {
            auto font = theme->GetTextFont();
            if (font && font->font()) {
                if (prompt_label_ &&
                    lv_obj_get_style_text_font(prompt_label_, LV_PART_MAIN) != font->font())
                    lv_obj_set_style_text_font(prompt_label_, font->font(), 0);
                if (message_label_ &&
                    lv_obj_get_style_text_font(message_label_, LV_PART_MAIN) != font->font())
                    lv_obj_set_style_text_font(message_label_, font->font(), 0);
            }
        }
        if (title && *title) {
            if (music_title_ != title) {
                music_artist_ = artist ? artist : "";
                music_title_ = title;
            }
        }
        if (artist && *artist && music_artist_ != artist)
            music_artist_ = artist;
        std::string head = music_title_;
        if (!music_artist_.empty()) {
            head += " · ";
            head += music_artist_;
        }
        if (prompt_label_)
            SetLabelTextIfChanged(prompt_label_, head.c_str());
        if (message_label_)
            SetLabelTextIfChanged(message_label_, (line && *line) ? line : "正在播放");
        if (button_label_)
            SetLabelTextIfChanged(button_label_, "继续对话");
        // Songs play through the radio page — keep lyric strip there in sync.
        if (apps_) {
            if (music_lyrics_.lines.empty())
                apps_->SetMusicLyric(music_title_.c_str(), music_artist_.c_str(), line);
            if (!apps_->IsRadioVisible())
                apps_->OpenRadio(false);
        }
    }

    void BeginMusicTrack(const char* title, const char* artist) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        music_title_ = title && *title ? title : "音乐";
        music_artist_ = artist ? artist : "";
        music_lyrics_ = {};
        music_line_index_ = -2;
        music_elapsed_ms_ = 0;
        music_resume_ms_ = 0;
        music_stream_open_sequence_ = 0;
        music_playing_ = false;
        music_active_ = true;
        music_seen_station_ = false;
        if (apps_)
            apps_->ClearMusicLyrics();
    }

    void UpdateMusicPlaybackState(const char* station, const char* state) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        UpdateMusicPlaybackStateLocked(station, state);
    }

    bool SetMusicLyrics(const char* lrc, const char* title = nullptr) {
        if (!lrc)
            return false;
        tab5_lrc::Document parsed;
        if (!tab5_lrc::Parse(lrc, parsed))
            return false;
        DisplayLockGuard lock(this);
        if (!lock.locked() || !music_active_ || (title && *title && music_title_ != title))
            return false;
        music_lyrics_ = std::move(parsed);
        music_line_index_ = -2;
        ESP_LOGI("Tab5Native", "music LRC lines=%u timed=%d", unsigned(music_lyrics_.lines.size()),
                 music_lyrics_.timed);
        RefreshMusicLyrics();
        return true;
    }

    void StopMusicTrack() {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        music_active_ = false;
        music_playing_ = false;
        music_seen_station_ = false;
        music_lyrics_ = {};
        if (apps_)
            apps_->ClearMusicLyrics();
    }

    void SetVisionPreview(const uint8_t* rgb) {
        DisplayLockGuard lock(this);
        if (!lock.locked() || !preview_image_)
            return;
        if (!rgb) {
            if (!preview_active_)
                return;
            preview_active_ = false;
            UpdateAnimationTimerPeriod();
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(preview_image_, nullptr);
            if (vision_preview_image_)
                lv_image_cache_drop(vision_preview_image_->image_dsc());
            vision_preview_image_.reset();
            vision_preview_pixels_ = nullptr;
            if (!sleeping_)
                lv_obj_remove_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            for (auto* dot : ambient_)
                if (dot && !sleeping_)
                    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            for (auto* z : sleep_z_)
                if (z && sleeping_)
                    lv_obj_remove_flag(z, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        constexpr size_t kPixels = 320 * 240;
        if (!vision_preview_image_) {
            if (preview_image_cached_) {
                lv_image_set_src(preview_image_, nullptr);
                lv_image_cache_drop(preview_image_cached_->image_dsc());
                preview_image_cached_.reset();
            }
            vision_preview_pixels_ = static_cast<uint16_t*>(
                heap_caps_malloc(kPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!vision_preview_pixels_)
                return;
            vision_preview_image_ = std::make_unique<LvglAllocatedImage>(
                vision_preview_pixels_, kPixels * sizeof(uint16_t), 320, 240, 320 * 2,
                LV_COLOR_FORMAT_RGB565);
        }
        for (size_t i = 0; i < kPixels; ++i) {
            const uint8_t* p = rgb + i * 3;
            vision_preview_pixels_[i] =
                uint16_t(((p[0] & 0xf8) << 8) | ((p[1] & 0xfc) << 3) | (p[2] >> 3));
        }
        if (preview_active_) {
            // RGB565 variable images are decoded directly; repaint from the reused buffer.
            lv_obj_invalidate(preview_image_);
            return;
        }
        lv_image_set_src(preview_image_, vision_preview_image_->image_dsc());
        preview_active_ = true;
        UpdateAnimationTimerPeriod();
        pending_emotion_.Clear();
        wave_active_ = greeting_active_ = false;
        pose_animation_.Stop();
        SetWaveRig(false);
        mouth_frame_ = eye_frame_ = 0;
        face_animation_.ResetEyes(lv_tick_get());
        for (auto* dot : thought_)
            SetVisible(dot, false);
        lv_obj_add_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
        for (auto* dot : ambient_)
            if (dot)
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        for (auto* z : sleep_z_)
            if (z)
                lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }

    void SetTheme(Theme* theme) override { Display::SetTheme(theme); }
};
