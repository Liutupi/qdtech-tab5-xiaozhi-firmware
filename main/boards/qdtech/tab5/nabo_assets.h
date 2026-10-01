#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif
extern const lv_image_dsc_t nabo_portrait;
extern const lv_image_dsc_t nabo_half_blink;
extern const lv_image_dsc_t nabo_closed_blink;
extern const lv_image_dsc_t nabo_wave_low;
extern const lv_image_dsc_t nabo_wave;
extern const lv_image_dsc_t nabo_wave_side;
extern const lv_image_dsc_t nabo_sleep;
extern const lv_image_dsc_t nabo_wave_legs;
extern const lv_image_dsc_t nabo_wave_torso;
extern const lv_image_dsc_t nabo_wave_head;
extern const lv_image_dsc_t nabo_wave_hand;
extern const lv_image_dsc_t nabo_wave_cuff;
extern const lv_image_dsc_t nabo_happy_legs;
extern const lv_image_dsc_t nabo_happy_torso;
extern const lv_image_dsc_t nabo_happy_head;
extern const lv_image_dsc_t nabo_think_legs;
extern const lv_image_dsc_t nabo_think_torso;
extern const lv_image_dsc_t nabo_listen_legs;
extern const lv_image_dsc_t nabo_listen_torso;
extern const lv_image_dsc_t nabo_listen_head;
extern const lv_image_dsc_t nabo_music_legs;
extern const lv_image_dsc_t nabo_music_torso;
extern const lv_image_dsc_t nabo_music_head;
extern const lv_image_dsc_t nabo_wink_legs;
extern const lv_image_dsc_t nabo_wink_torso;
extern const lv_image_dsc_t nabo_wink_hand;
extern const lv_image_dsc_t nabo_wink_cuff;
extern const lv_image_dsc_t nabo_encourage_legs;
extern const lv_image_dsc_t nabo_encourage_torso;
extern const lv_image_dsc_t nabo_encourage_hand;
extern const lv_image_dsc_t nabo_encourage_cuff;
extern const lv_image_dsc_t nabo_curious_legs;
extern const lv_image_dsc_t nabo_curious_torso;
extern const lv_image_dsc_t nabo_comfort_legs;
extern const lv_image_dsc_t nabo_comfort_torso;
extern const lv_image_dsc_t nabo_mouth_half;
extern const lv_image_dsc_t nabo_mouth_open;
extern const lv_image_dsc_t* const nabo_wave_v2_frames[8];
extern const lv_image_dsc_t* const nabo_reaction_frames[8];
extern const unsigned char nabo_greeting_ogg[];
extern const unsigned int nabo_greeting_ogg_len;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "nabo_animation.h"
inline constexpr nabo::WaveGeometry NaboWaveGeometry() {
    return {{57, 380, 93, 181}, {14, 222, 136, 205}, {20, 0, 131, 235}, {-3, 183, 40, 67}, {10, 228, 27, 22}, -200};
}
struct NaboReactionLayers {
    const lv_image_dsc_t *legs, *torso, *head, *hand, *cuff;
    nabo::WaveGeometry geometry;
};
inline constexpr NaboReactionLayers NaboReactionRig(nabo::Action action) {
    switch (action) {
    case nabo::Action::Happy:
        return {&nabo_happy_legs, &nabo_happy_torso, &nabo_happy_head, nullptr, nullptr, {{62, 420, 88, 141}, {59, 266, 91, 179}, {32, 37, 120, 242}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0}};
    case nabo::Action::Think:
        return {&nabo_think_legs, &nabo_think_torso, nullptr, nullptr, nullptr, {{42, 419, 108, 142}, {11, 38, 139, 407}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0}};
    case nabo::Action::Listen:
        return {&nabo_listen_legs, &nabo_listen_torso, &nabo_listen_head, nullptr, nullptr, {{73, 418, 77, 143}, {23, 212, 131, 233}, {19, 53, 146, 231}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0}};
    case nabo::Action::Music:
        return {&nabo_music_legs, &nabo_music_torso, &nabo_music_head, nullptr, nullptr, {{63, 416, 87, 145}, {2, 217, 149, 221}, {24, 63, 123, 220}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0}};
    case nabo::Action::Wink:
        return {&nabo_wink_legs, &nabo_wink_torso, nullptr, &nabo_wink_hand, &nabo_wink_cuff, {{63, 418, 87, 143}, {22, 37, 128, 407}, {0, 0, 0, 0}, {28, 243, 42, 84}, {52, 319, 18, 8}, 0}};
    case nabo::Action::Encourage:
        return {&nabo_encourage_legs, &nabo_encourage_torso, nullptr, &nabo_encourage_hand, &nabo_encourage_cuff, {{40, 418, 110, 143}, {0, 37, 149, 408}, {0, 0, 0, 0}, {15, 247, 27, 82}, {16, 315, 26, 14}, 0}};
    case nabo::Action::Curious:
        return {&nabo_curious_legs, &nabo_curious_torso, nullptr, nullptr, nullptr, {{68, 412, 82, 149}, {17, 61, 143, 379}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0}};
    case nabo::Action::Comfort:
        return {&nabo_comfort_legs, &nabo_comfort_torso, nullptr, nullptr, nullptr, {{51, 418, 99, 143}, {3, 55, 148, 390}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, 0}};
    default: return {nullptr, nullptr, nullptr, nullptr, nullptr, {}};
    }
}
#endif
