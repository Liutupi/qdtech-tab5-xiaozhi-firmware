#include "usb_gamepad_host.h"

#include <atomic>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

namespace {

constexpr char kTag[] = "UsbGamepad";

std::atomic<uint8_t> s_nes_mask{0};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_xinput{false};
std::atomic<bool> s_ds4{false};
void (*s_on_connect)() = nullptr;
QueueHandle_t s_event_queue = nullptr;

// Xbox 360 / SN30 Pro X-input (vid 045e pid 028e) input report:
// [0]=0x00 type, [1]=0x14 len, [2]=dpad/start bits, [3]=face/LB/RB,
// [4]=LT, [5]=RT, [6..7]=LX, [8..9]=LY (int16 le).
uint8_t ParseX360Report(const uint8_t* data, int length) {
    if (!data || length < 10) return 0;
    // Accept both 20-byte classic and slightly shorter variants.
    const uint8_t b2 = data[2];
    const uint8_t b3 = data[3];
    uint8_t nes = 0;
    if (b2 & 0x01) nes |= kNesBtnUp;
    if (b2 & 0x02) nes |= kNesBtnDown;
    if (b2 & 0x04) nes |= kNesBtnLeft;
    if (b2 & 0x08) nes |= kNesBtnRight;
    if (b2 & 0x10) nes |= kNesBtnStart;
    if (b2 & 0x20) nes |= kNesBtnSelect;
    if (b3 & 0x10) nes |= kNesBtnA;   // A
    if (b3 & 0x20) nes |= kNesBtnB;   // B
    if (b3 & 0x40) nes |= kNesBtnA;   // X also maps to A
    // Left stick as D-pad fallback.
    if (length >= 9) {
        const int16_t lx = int16_t(uint16_t(data[6]) | (uint16_t(data[7]) << 8));
        const int16_t ly = int16_t(uint16_t(data[8]) | (uint16_t(data[9]) << 8));
        if (lx > 8000) nes |= kNesBtnRight;
        if (lx < -8000) nes |= kNesBtnLeft;
        if (ly > 8000) nes |= kNesBtnUp;
        if (ly < -8000) nes |= kNesBtnDown;
    }
    return nes;
}

struct HostEvent {
    enum class Kind : uint8_t { Lib, Device };
    Kind kind = Kind::Lib;
    hid_host_device_handle_t device = nullptr;
    hid_host_driver_event_t device_event{};
};

// NES bits: A, B, Select, Start, Up, Down, Left, Right.
// HID gamepad (D-input / 8BitDo Android) common button indices:
// 0 South, 1 East, 2 West, 3 North, 4 L1, 5 R1, 6 L2, 7 R2, 8 Select, 9 Start.
uint8_t MapButtonsAndHat(uint32_t buttons, int hat) {
    uint8_t nes = 0;
    // Nintendo layout: face-right is NES A, face-bottom is NES B.
    if (buttons & (1u << 1)) nes |= kNesBtnA;   // East / B-circle / face-right
    if (buttons & (1u << 0)) nes |= kNesBtnB;   // South / A-cross / face-bottom
    if (buttons & (1u << 2)) nes |= kNesBtnA;   // West treated as A as well
    if (buttons & (1u << 8)) nes |= kNesBtnSelect;
    if (buttons & (1u << 9)) nes |= kNesBtnStart;
    // Some pads use buttons 6/7 as Start/Select.
    if (buttons & (1u << 6)) nes |= kNesBtnSelect;
    if (buttons & (1u << 7)) nes |= kNesBtnStart;

    if (hat >= 0 && hat <= 7) {
        // 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW
        if (hat == 0 || hat == 1 || hat == 7) nes |= kNesBtnUp;
        if (hat == 2 || hat == 1 || hat == 3) nes |= kNesBtnRight;
        if (hat == 4 || hat == 3 || hat == 5) nes |= kNesBtnDown;
        if (hat == 6 || hat == 5 || hat == 7) nes |= kNesBtnLeft;
    }
    return nes;
}

uint8_t AxisToDpad(int value, int center) {
    // value is roughly 0..255 or -127..127 depending on report; caller centers.
    if (value > center + 48) return kNesBtnRight;
    if (value < center - 48) return kNesBtnLeft;
    return 0;
}

uint8_t AxisToDpadY(int value, int center) {
    // USB HID Y: up is typically smaller than center.
    if (value < center - 48) return kNesBtnUp;
    if (value > center + 48) return kNesBtnDown;
    return 0;
}

// Parse a generic HID gamepad input report into NES bits.
// Handles the common 8BitDo / SN30 Pro D-input layout and a few close variants.
uint8_t ParseGamepadReport(const uint8_t* data, int length) {
    if (!data || length < 2) return 0;

    // Layout A (common 8BitDo Android / HID):
    // [0]=buttons0-7, [1]=buttons8-15, [2]=hat nibble + pad, [3]=X, [4]=Y, ...
    // Layout B (some pads, report id 1): [0]=id, [1..]=same as A
    int offset = 0;
    if (length >= 9 && data[0] == 0x01 && (data[1] != 0 || data[2] != 0 || data[3] < 16)) {
        // Heuristic: report-id prefix when first byte is a small id.
        // Only shift if remaining looks more like buttons/hat than axes.
        if (length >= 10 && data[2] <= 0x0f) {
            offset = 1;
        }
    }

    const int n = length - offset;
    const uint8_t* p = data + offset;
    if (n < 4) return 0;

    uint32_t buttons = 0;
    buttons |= uint32_t(p[0]);
    if (n > 1) buttons |= uint32_t(p[1]) << 8;
    if (n > 2) buttons |= uint32_t(p[2]) << 16;

    int hat = -1;
    // Hat is often the low nibble of byte 2, or byte 2 alone (0-7 / 0x0f released).
    if (n > 2) {
        const uint8_t b2 = p[2];
        if (b2 <= 7) {
            hat = b2;
            buttons &= ~0x00ff00u;  // don't treat hat bits as buttons
        } else if ((b2 & 0x0f) <= 7) {
            hat = b2 & 0x0f;
        } else if (b2 == 0x0f || b2 == 0x8f) {
            hat = -1;
        }
    }

    uint8_t nes = MapButtonsAndHat(buttons, hat);

    // Left stick axes as D-pad fallback when hat is idle.
    // Common: byte3=X, byte4=Y after buttons/hat.
    int x_byte = 3, y_byte = 4;
    if (n >= 8) {
        const int x = p[x_byte];
        const int y = p[y_byte];
        // 8-bit centered axes (0..255, 128 center)
        nes |= AxisToDpad(x, 128);
        nes |= AxisToDpadY(y, 128);
        // If those were not centered axes (values already near 0/255 extremes as
        // digital), also try 16-bit little-endian axes in bytes 3..6.
        if (n >= 7 && (p[5] == 0 || p[6] == 0 || p[5] == 0xff || p[6] == 0xff)) {
            const int x16 = int(p[3]) | (int(p[4]) << 8);
            const int y16 = int(p[5]) | (int(p[6]) << 8);
            if (x16 != 0 || y16 != 0) {
                nes |= AxisToDpad(x16 > 32767 ? x16 - 65536 : x16, 0);
                nes |= AxisToDpadY(y16 > 32767 ? y16 - 65536 : y16, 0);
            }
        }
    }

    return nes;
}

// Sony DualShock 4 USB input report (report id 0x01):
// [0]=id, [1]=LX, [2]=LY, [3]=RX, [4]=RY,
// [5]=dpad nibble + face buttons, [6]=L1/R1/L2/R2/share/options/L3/R3.
uint8_t ParseDs4Report(const uint8_t* data, int length) {
    if (!data || length < 7) return 0;
    int o = 0;
    if (data[0] == 0x01) o = 1;
    if (length < o + 6) return 0;
    const uint8_t b5 = data[o + 4];
    const uint8_t b6 = data[o + 5];
    uint8_t nes = 0;
    const int dpad = b5 & 0x0f;
    if (dpad == 0 || dpad == 1 || dpad == 7) nes |= kNesBtnUp;
    if (dpad == 2 || dpad == 1 || dpad == 3) nes |= kNesBtnRight;
    if (dpad == 4 || dpad == 3 || dpad == 5) nes |= kNesBtnDown;
    if (dpad == 6 || dpad == 5 || dpad == 7) nes |= kNesBtnLeft;
    if (b5 & 0x20) nes |= kNesBtnB;  // Cross
    if (b5 & 0x40) nes |= kNesBtnA;  // Circle
    if (b5 & 0x10) nes |= kNesBtnA;  // Square also A
    if (b6 & 0x10) nes |= kNesBtnSelect;  // Share
    if (b6 & 0x20) nes |= kNesBtnStart;   // Options
    // Left stick
    const int lx = int(data[o]) - 128;
    const int ly = int(data[o + 1]) - 128;
    if (lx > 48) nes |= kNesBtnRight;
    if (lx < -48) nes |= kNesBtnLeft;
    if (ly < -48) nes |= kNesBtnUp;
    if (ly > 48) nes |= kNesBtnDown;
    return nes;
}

void OnGamepadReport(const uint8_t* data, int length) {
    if (!data || length <= 0) return;
    uint8_t nes = 0;
    if (s_xinput.load(std::memory_order_relaxed)) {
        nes = ParseX360Report(data, length);
    } else if (s_ds4.load(std::memory_order_relaxed)) {
        nes = ParseDs4Report(data, length);
    } else {
        nes = ParseGamepadReport(data, length);
        if (nes == 0 && length >= 7) {
            nes = ParseDs4Report(data, length);
        }
    }
    s_nes_mask.store(nes, std::memory_order_relaxed);
    static uint8_t last = 0xff;
    // Log only when the mask actually changes — idle traffic must not flood.
    if (nes != last) {
        last = nes;
        ESP_LOGD(kTag, "gamepad nes=0x%02x", nes);
    }
}

void InterfaceCallback(hid_host_device_handle_t handle, const hid_host_interface_event_t event,
                       void* arg) {
    (void)arg;
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64] = {0};
            size_t data_length = 0;
            if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data),
                                                         &data_length) == ESP_OK &&
                data_length > 0) {
                OnGamepadReport(data, static_cast<int>(data_length));
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGW(kTag, "HID gamepad disconnected");
            s_connected.store(false);
            s_nes_mask.store(0);
            hid_host_device_close(handle);
            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(kTag, "HID transfer error");
            break;
        default:
            break;
    }
}

void DeviceEvent(hid_host_device_handle_t handle, hid_host_driver_event_t event) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }
    // USB endpoints need contiguous internal SRAM. Free camera DMA first.
    if (s_on_connect) {
        s_on_connect();
    }
    ESP_LOGI(kTag, "internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    hid_host_dev_params_t params{};
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        ESP_LOGE(kTag, "hid get_params failed");
        return;
    }
    hid_host_dev_info_t info{};
    if (hid_host_get_device_info(handle, &info) == ESP_OK) {
        ESP_LOGI(kTag, "HID device connected proto=%d subclass=%d vid=%04x pid=%04x",
                 int(params.proto), int(params.sub_class), unsigned(info.VID), unsigned(info.PID));
        if (info.VID == 0x054c) {
            s_ds4.store(true);
            ESP_LOGI(kTag, "Sony DS4-style report layout enabled");
        }
    } else {
        ESP_LOGI(kTag, "HID device connected proto=%d subclass=%d", int(params.proto),
                 int(params.sub_class));
    }

    const hid_host_device_config_t cfg = {
        .callback = InterfaceCallback,
        .callback_arg = nullptr,
    };
    // Open boot keyboard/mouse AND generic gamepads (proto NONE).
    esp_err_t err = hid_host_device_open(handle, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "hid_host_device_open failed: %s", esp_err_to_name(err));
        return;
    }
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
    }
    // Keep the interrupt pipe alive; some pads only stream after SET_IDLE 0.
    hid_class_request_set_idle(handle, 0, 0);
    err = hid_host_device_start(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "hid_host_device_start failed: %s", esp_err_to_name(err));
        hid_host_device_close(handle);
        return;
    }
    s_connected.store(true);
    ESP_LOGI(kTag, "USB gamepad ready (SN30 Pro / HID)");
}

void HidDeviceCallback(hid_host_device_handle_t handle, const hid_host_driver_event_t event,
                       void* arg) {
    (void)arg;
    if (!s_event_queue) return;
    HostEvent ev;
    ev.kind = HostEvent::Kind::Device;
    ev.device = handle;
    ev.device_event = event;
    xQueueSend(s_event_queue, &ev, 0);
}

usb_host_client_handle_t s_inspect_client = nullptr;
usb_transfer_t* s_x360_xfer = nullptr;
usb_device_handle_t s_x360_dev = nullptr;

void X360TransferCallback(usb_transfer_t* xfer) {
    if (!xfer) return;
    if (xfer->status == 0 && xfer->actual_num_bytes > 0) {
        OnGamepadReport(xfer->data_buffer, xfer->actual_num_bytes);
    } else if (xfer->status != 0) {
        static int err_log = 0;
        if ((err_log++ % 32) == 0) {
            ESP_LOGW(kTag, "x360 xfer status=%d bytes=%d", xfer->status, xfer->actual_num_bytes);
        }
    }
    if (s_x360_dev && s_x360_xfer) {
        usb_host_transfer_submit(s_x360_xfer);
    }
}

void StartX360(usb_device_handle_t dev, usb_host_client_handle_t client) {
    if (s_x360_xfer) return;

    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !cfg) {
        ESP_LOGE(kTag, "x360 no config descriptor");
        return;
    }

    // Walk interfaces/endpoints and take the first interrupt IN endpoint.
    int offset = 0;
    const usb_intf_desc_t* intf = nullptr;
    uint8_t ep_addr = 0;
    uint8_t intf_num = 0;
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        offset = 0;
        intf = usb_parse_interface_descriptor(cfg, i, 0, &offset);
        if (!intf) continue;
        for (int e = 0; e < intf->bNumEndpoints; ++e) {
            const usb_ep_desc_t* ep =
                usb_parse_endpoint_descriptor_by_index(intf, e, cfg->wTotalLength, &offset);
            if (!ep) break;
            const bool is_in = (ep->bEndpointAddress & 0x80) != 0;
            const auto xfer = USB_EP_DESC_GET_XFERTYPE(ep);
            if (is_in && xfer == USB_TRANSFER_TYPE_INTR) {
                ep_addr = ep->bEndpointAddress;
                intf_num = intf->bInterfaceNumber;
                ESP_LOGI(kTag, "x360 using iface=%u ep=0x%02x mps=%u", unsigned(intf_num),
                         unsigned(ep_addr), unsigned(USB_EP_DESC_GET_MPS(ep)));
                break;
            }
        }
        if (ep_addr) break;
    }
    if (!ep_addr) {
        ESP_LOGE(kTag, "x360 no interrupt IN endpoint found");
        return;
    }

    if (usb_host_interface_claim(client, dev, intf_num, 0) != ESP_OK) {
        ESP_LOGE(kTag, "x360 claim iface=%u failed", unsigned(intf_num));
        return;
    }
    if (usb_host_transfer_alloc(64, 0, &s_x360_xfer) != ESP_OK) {
        ESP_LOGE(kTag, "x360 transfer alloc failed");
        return;
    }
    s_x360_xfer->device_handle = dev;
    s_x360_xfer->bEndpointAddress = ep_addr;
    s_x360_xfer->callback = X360TransferCallback;
    s_x360_xfer->context = nullptr;
    s_x360_xfer->num_bytes = 64;
    s_x360_dev = dev;
    s_xinput.store(true);
    s_connected.store(true);
    esp_err_t err = usb_host_transfer_submit(s_x360_xfer);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "x360 transfer submit failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(kTag, "X-input (Xbox360/SN30) poller started ep=0x%02x", unsigned(ep_addr));
}

void UsbClientEvent(const usb_host_client_event_msg_t* msg, void* arg) {
    (void)arg;
    if (!msg || msg->event != USB_HOST_CLIENT_EVENT_NEW_DEV || !s_inspect_client) return;
    usb_device_handle_t dev = nullptr;
    if (usb_host_device_open(s_inspect_client, msg->new_dev.address, &dev) != ESP_OK) {
        ESP_LOGW(kTag, "usb open addr=%u failed", unsigned(msg->new_dev.address));
        return;
    }
    const usb_device_desc_t* desc = nullptr;
    if (usb_host_get_device_descriptor(dev, &desc) == ESP_OK && desc) {
        ESP_LOGI(kTag,
                 "USB device addr=%u class=0x%02x subclass=0x%02x proto=0x%02x vid=%04x pid=%04x",
                 unsigned(msg->new_dev.address), unsigned(desc->bDeviceClass),
                 unsigned(desc->bDeviceSubClass), unsigned(desc->bDeviceProtocol),
                 unsigned(desc->idVendor), unsigned(desc->idProduct));
        if (desc->idVendor == 0x045e && desc->idProduct == 0x028e) {
            StartX360(dev, s_inspect_client);
            return;  // keep device open for the poller
        }
    }
    usb_host_device_close(s_inspect_client, dev);
}

void UsbLibTask(void*) {
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    // FC/audio can starve internal SRAM; keep retrying instead of giving up
    // forever (UsbGamepadHostStart used to latch and never try again).
    for (int attempt = 0; err != ESP_OK && err != ESP_ERR_INVALID_STATE; ++attempt) {
        ESP_LOGE(kTag, "usb_host_install failed: %s (attempt %d)", esp_err_to_name(err),
                 attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        err = usb_host_install(&host_cfg);
    }
    ESP_LOGI(kTag, "USB host installed");
    // Do not open devices here — a second client steals the HID interface
    // and the pad never reaches hid_host. Just pump host events.

    int last_devices = -1;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
        usb_host_lib_info_t info{};
        if (usb_host_lib_info(&info) == ESP_OK && info.num_devices != last_devices) {
            last_devices = info.num_devices;
            ESP_LOGI(kTag, "USB enumerated devices=%d clients=%d", info.num_devices, info.num_clients);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void HidTask(void*) {
    s_event_queue = xQueueCreate(8, sizeof(HostEvent));
    if (!s_event_queue) {
        vTaskDelete(nullptr);
        return;
    }

    // Give usb_lib_task a moment to install the host.
    vTaskDelay(pdMS_TO_TICKS(200));

    hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = HidDeviceCallback,
        .callback_arg = nullptr,
    };
    esp_err_t err = hid_host_install(&hid_cfg);
    for (int attempt = 0; err != ESP_OK; ++attempt) {
        ESP_LOGE(kTag, "hid_host_install failed: %s (attempt %d)", esp_err_to_name(err),
                 attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        err = hid_host_install(&hid_cfg);
    }
    ESP_LOGI(kTag, "HID host installed, waiting for gamepad");

    HostEvent ev;
    while (true) {
        if (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE &&
            ev.kind == HostEvent::Kind::Device) {
            DeviceEvent(ev.device, ev.device_event);
        }
    }
}

}  // namespace

bool UsbGamepadHostStart() {
    static bool tasks_spawned = false;
    if (tasks_spawned) return true;

    if (xTaskCreatePinnedToCore(UsbLibTask, "usb_lib", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(kTag, "usb_lib task failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(HidTask, "usb_hid", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
        ESP_LOGE(kTag, "usb_hid task failed");
        return false;
    }
    // Tasks retry usb_host_install / hid_host_install internally.
    tasks_spawned = true;
    return true;
}

void UsbGamepadSetOnConnect(void (*cb)()) { s_on_connect = cb; }

uint8_t UsbGamepadNesMask() { return s_nes_mask.load(std::memory_order_relaxed); }

bool UsbGamepadConnected() { return s_connected.load(std::memory_order_relaxed); }
