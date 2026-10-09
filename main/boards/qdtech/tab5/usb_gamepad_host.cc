#include "usb_gamepad_host.h"
#include "tab5_gamepad_report.h"

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
std::atomic<uint8_t> s_md_mask{0};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_xinput{false};
std::atomic<bool> s_ds4{false};
void (*s_on_connect)() = nullptr;
QueueHandle_t s_event_queue = nullptr;

struct HostEvent {
    enum class Kind : uint8_t { Lib, Device };
    Kind kind = Kind::Lib;
    hid_host_device_handle_t device = nullptr;
    hid_host_driver_event_t device_event{};
};

void OnGamepadReport(const uint8_t* data, int length) {
    if (!data || length <= 0) return;
    const auto layout = s_xinput.load(std::memory_order_relaxed) ? tab5_gamepad::Layout::Xinput
                        : s_ds4.load(std::memory_order_relaxed)  ? tab5_gamepad::Layout::Ds4
                                                                 : tab5_gamepad::Layout::Hid;
    const auto report = tab5_gamepad::Parse(layout, data, length);
    const uint8_t nes = report.nes;
    s_nes_mask.store(nes, std::memory_order_relaxed);
    s_md_mask.store(report.md, std::memory_order_relaxed);
    static uint8_t last = 0xff, last_md = 0xff;
    // Log only when the mask actually changes — idle traffic must not flood.
    if (nes != last || report.md != last_md) {
        last = nes;
        last_md = report.md;
        ESP_LOGI(kTag, "gamepad nes=0x%02x md=0x%02x", nes, report.md);
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
            s_md_mask.store(0);
            s_ds4.store(false);
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
bool s_x360_gone = false;
bool s_x360_pending = false;
uint8_t s_x360_interface = 0;
std::atomic<bool> s_host_ready{false};

void X360TransferCallback(usb_transfer_t* xfer) {
    if (!xfer) return;
    s_x360_pending = false;
    if (s_x360_gone)
        return;
    if (xfer->status == 0 && xfer->actual_num_bytes > 0) {
        OnGamepadReport(xfer->data_buffer, xfer->actual_num_bytes);
    } else if (xfer->status != 0) {
        static int err_log = 0;
        if ((err_log++ % 32) == 0) {
            ESP_LOGW(kTag, "x360 xfer status=%d bytes=%d", xfer->status, xfer->actual_num_bytes);
        }
    }
    if (s_x360_dev && s_x360_xfer) {
        s_x360_pending = usb_host_transfer_submit(s_x360_xfer) == ESP_OK;
    }
}

bool StartX360(usb_device_handle_t dev, usb_host_client_handle_t client) {
    if (s_x360_xfer)
        return false;

    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || !cfg) {
        ESP_LOGE(kTag, "x360 no config descriptor");
        return false;
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
        return false;
    }

    if (usb_host_interface_claim(client, dev, intf_num, 0) != ESP_OK) {
        ESP_LOGE(kTag, "x360 claim iface=%u failed", unsigned(intf_num));
        return false;
    }
    s_x360_interface = intf_num;
    if (usb_host_transfer_alloc(64, 0, &s_x360_xfer) != ESP_OK) {
        usb_host_interface_release(client, dev, intf_num);
        ESP_LOGE(kTag, "x360 transfer alloc failed");
        return false;
    }
    s_x360_xfer->device_handle = dev;
    s_x360_xfer->bEndpointAddress = ep_addr;
    s_x360_xfer->callback = X360TransferCallback;
    s_x360_xfer->context = nullptr;
    s_x360_xfer->num_bytes = 64;
    s_x360_dev = dev;
    s_x360_gone = false;
    s_xinput.store(true);
    esp_err_t err = usb_host_transfer_submit(s_x360_xfer);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "x360 transfer submit failed: %s", esp_err_to_name(err));
        usb_host_transfer_free(s_x360_xfer);
        s_x360_xfer = nullptr;
        s_x360_dev = nullptr;
        s_xinput.store(false);
        usb_host_interface_release(client, dev, intf_num);
        return false;
    }
    s_x360_pending = true;
    s_connected.store(true);
    ESP_LOGI(kTag, "X-input (Xbox360/SN30) poller started ep=0x%02x", unsigned(ep_addr));
    return true;
}

void UsbClientEvent(const usb_host_client_event_msg_t* msg, void* arg) {
    (void)arg;
    if (!msg || !s_inspect_client)
        return;
    if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (msg->dev_gone.dev_hdl == s_x360_dev) {
            s_x360_gone = true;
            s_connected.store(false);
            s_nes_mask.store(0);
            s_md_mask.store(0);
            usb_host_endpoint_halt(s_x360_dev, s_x360_xfer->bEndpointAddress);
            usb_host_endpoint_flush(s_x360_dev, s_x360_xfer->bEndpointAddress);
        }
        return;
    }
    if (msg->event != USB_HOST_CLIENT_EVENT_NEW_DEV)
        return;
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
            if (s_on_connect)
                s_on_connect();
            if (StartX360(dev, s_inspect_client))
                return;  // only this vendor interface is claimed
        }
    }
    usb_host_device_close(s_inspect_client, dev);
}

void InspectTask(void*) {
    while (!s_host_ready.load())
        vTaskDelay(pdMS_TO_TICKS(50));
    usb_host_client_config_t cfg = {};
    cfg.is_synchronous = false;
    cfg.max_num_event_msg = 8;
    cfg.async.client_event_callback = UsbClientEvent;
    for (;;) {
        const auto err = usb_host_client_register(&cfg, &s_inspect_client);
        if (err == ESP_OK)
            break;
        ESP_LOGW(kTag, "gamepad inspector failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    for (;;) {
        usb_host_client_handle_events(s_inspect_client, pdMS_TO_TICKS(100));
        if (s_x360_gone && !s_x360_pending && s_x360_dev) {
            usb_host_interface_release(s_inspect_client, s_x360_dev, s_x360_interface);
            usb_host_transfer_free(s_x360_xfer);
            s_x360_xfer = nullptr;
            usb_host_device_close(s_inspect_client, s_x360_dev);
            s_x360_dev = nullptr;
            s_xinput.store(false);
            s_x360_gone = false;
            ESP_LOGI(kTag, "X-input gamepad released; ready to reconnect");
        }
    }
}

void UsbLibTask(void*) {
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    esp_err_t err = usb_host_install(&host_cfg);
    // FC/audio can starve internal SRAM; keep retrying instead of giving up
    // forever (UsbGamepadHostStart used to latch and never try again).
    for (int attempt = 0; err != ESP_OK && err != ESP_ERR_INVALID_STATE; ++attempt) {
        ESP_LOGE(kTag, "usb_host_install failed: %s (attempt %d)", esp_err_to_name(err),
                 attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        err = usb_host_install(&host_cfg);
    }
    s_host_ready.store(true);
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
    while (!s_event_queue) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        s_event_queue = xQueueCreate(8, sizeof(HostEvent));
    }

    while (!s_host_ready.load())
        vTaskDelay(pdMS_TO_TICKS(50));

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
    // Track each task separately: a partial allocation failure must not create
    // another host pump on retry, or leave the X-input client permanently absent.
    static TaskHandle_t lib = nullptr, hid = nullptr, inspect = nullptr;
    if (!lib && xTaskCreatePinnedToCore(UsbLibTask, "usb_lib", 4096, nullptr, 5, &lib, 0) != pdPASS)
        return false;
    if (!hid && xTaskCreatePinnedToCore(HidTask, "usb_hid", 4096, nullptr, 5, &hid, 0) != pdPASS)
        return false;
    if (!inspect &&
        xTaskCreatePinnedToCore(InspectTask, "usb_pad", 4096, nullptr, 5, &inspect, 0) != pdPASS)
        return false;
    return true;
}

void UsbGamepadSetOnConnect(void (*cb)()) { s_on_connect = cb; }

uint8_t UsbGamepadNesMask() { return s_nes_mask.load(std::memory_order_relaxed); }

uint8_t UsbGamepadMdMask() { return s_md_mask.load(std::memory_order_relaxed); }

bool UsbGamepadConnected() { return s_connected.load(std::memory_order_relaxed); }
