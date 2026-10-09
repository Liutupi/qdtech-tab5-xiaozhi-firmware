#include "md_controls.h"
#include <atomic>
#include "usb_gamepad_host.h"
extern "C" {
#include "md_board.h"
}
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
std::atomic<bool> exiting{false};
void TouchTask(void*) {
    bool armed = false;
    for (;;) {
        uint16_t x = 0, y = 0;
        const bool down = md_board_touch(&x, &y);
        const bool inside = down && x >= 24 && x < 136 && y >= 36 && y < 96;
        if (armed && !down)
            exiting.store(true);
        if (down)
            armed = inside;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}  // namespace

bool md_controls_init(void) {
    const bool pad = UsbGamepadHostStart();
    if (!pad)
        ESP_LOGW("md_controls", "USB tasks unavailable; touch exit remains active");
    TaskHandle_t touch = nullptr;
    return xTaskCreate(TouchTask, "md_touch", 3072, nullptr, 4, &touch) == pdPASS;
}
uint8_t md_controls_buttons(void) { return UsbGamepadMdMask(); }
bool md_controls_exit(void) {
    constexpr uint8_t combo = kNesBtnSelect | kNesBtnStart;
    return exiting.load() || (UsbGamepadNesMask() & combo) == combo;
}
