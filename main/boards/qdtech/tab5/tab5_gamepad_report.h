#pragma once

#include "usb_gamepad_host.h"

namespace tab5_gamepad {

// Xbox 360 / SN30 Pro X-input (vid 045e pid 028e) input report:
// [0]=0x00 type, [1]=0x14 len, [2]=dpad/start bits, [3]=face/LB/RB,
// [4]=LT, [5]=RT, [6..7]=LX, [8..9]=LY (int16 le).
inline uint8_t ParseX360Report(const uint8_t* data, int length) {
    if (!data || length < 10)
        return 0;
    // Accept both 20-byte classic and slightly shorter variants.
    const uint8_t b2 = data[2];
    const uint8_t b3 = data[3];
    uint8_t nes = 0;
    if (b2 & 0x01)
        nes |= kNesBtnUp;
    if (b2 & 0x02)
        nes |= kNesBtnDown;
    if (b2 & 0x04)
        nes |= kNesBtnLeft;
    if (b2 & 0x08)
        nes |= kNesBtnRight;
    if (b2 & 0x10)
        nes |= kNesBtnStart;
    if (b2 & 0x20)
        nes |= kNesBtnSelect;
    if (b3 & 0x10)
        nes |= kNesBtnA;  // A
    if (b3 & 0x20)
        nes |= kNesBtnB;  // B
    if (b3 & 0x40)
        nes |= kNesBtnA;  // X also maps to A
    // Left stick as D-pad fallback.
    if (length >= 9) {
        const int16_t lx = int16_t(uint16_t(data[6]) | (uint16_t(data[7]) << 8));
        const int16_t ly = int16_t(uint16_t(data[8]) | (uint16_t(data[9]) << 8));
        if (lx > 8000)
            nes |= kNesBtnRight;
        if (lx < -8000)
            nes |= kNesBtnLeft;
        if (ly > 8000)
            nes |= kNesBtnUp;
        if (ly < -8000)
            nes |= kNesBtnDown;
    }
    return nes;
}

// NES bits: A, B, Select, Start, Up, Down, Left, Right.
// HID gamepad (D-input / 8BitDo Android) common button indices:
// 0 South, 1 East, 2 West, 3 North, 4 L1, 5 R1, 6 L2, 7 R2, 8 Select, 9 Start.
inline uint8_t MapButtonsAndHat(uint32_t buttons, int hat) {
    uint8_t nes = 0;
    // Nintendo layout: face-right is NES A, face-bottom is NES B.
    if (buttons & (1u << 1))
        nes |= kNesBtnA;  // East / B-circle / face-right
    if (buttons & (1u << 0))
        nes |= kNesBtnB;  // South / A-cross / face-bottom
    if (buttons & (1u << 2))
        nes |= kNesBtnA;  // West treated as A as well
    if (buttons & (1u << 8))
        nes |= kNesBtnSelect;
    if (buttons & (1u << 9))
        nes |= kNesBtnStart;
    // Some pads use buttons 6/7 as Start/Select.
    if (buttons & (1u << 6))
        nes |= kNesBtnSelect;
    if (buttons & (1u << 7))
        nes |= kNesBtnStart;

    if (hat >= 0 && hat <= 7) {
        // 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW
        if (hat == 0 || hat == 1 || hat == 7)
            nes |= kNesBtnUp;
        if (hat == 2 || hat == 1 || hat == 3)
            nes |= kNesBtnRight;
        if (hat == 4 || hat == 3 || hat == 5)
            nes |= kNesBtnDown;
        if (hat == 6 || hat == 5 || hat == 7)
            nes |= kNesBtnLeft;
    }
    return nes;
}

inline uint8_t AxisToDpad(int value, int center) {
    // value is roughly 0..255 or -127..127 depending on report; caller centers.
    if (value > center + 48)
        return kNesBtnRight;
    if (value < center - 48)
        return kNesBtnLeft;
    return 0;
}

inline uint8_t AxisToDpadY(int value, int center) {
    // USB HID Y: up is typically smaller than center.
    if (value < center - 48)
        return kNesBtnUp;
    if (value > center + 48)
        return kNesBtnDown;
    return 0;
}

// Parse a generic HID gamepad input report into NES bits.
// Handles the common 8BitDo / SN30 Pro D-input layout and a few close variants.
inline uint8_t ParseGamepadReport(const uint8_t* data, int length) {
    if (!data || length < 2)
        return 0;

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
    if (n < 4)
        return 0;

    uint32_t buttons = 0;
    buttons |= uint32_t(p[0]);
    if (n > 1)
        buttons |= uint32_t(p[1]) << 8;
    if (n > 2)
        buttons |= uint32_t(p[2]) << 16;

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
    }

    return nes;
}

// Sony DualShock 4 USB input report (report id 0x01):
// [0]=id, [1]=LX, [2]=LY, [3]=RX, [4]=RY,
// [5]=dpad nibble + face buttons, [6]=L1/R1/L2/R2/share/options/L3/R3.
inline uint8_t ParseDs4Report(const uint8_t* data, int length) {
    if (!data || length < 7)
        return 0;
    int o = 0;
    if (data[0] == 0x01)
        o = 1;
    if (length < o + 6)
        return 0;
    const uint8_t b5 = data[o + 4];
    const uint8_t b6 = data[o + 5];
    uint8_t nes = 0;
    const int dpad = b5 & 0x0f;
    if (dpad == 0 || dpad == 1 || dpad == 7)
        nes |= kNesBtnUp;
    if (dpad == 2 || dpad == 1 || dpad == 3)
        nes |= kNesBtnRight;
    if (dpad == 4 || dpad == 3 || dpad == 5)
        nes |= kNesBtnDown;
    if (dpad == 6 || dpad == 5 || dpad == 7)
        nes |= kNesBtnLeft;
    if (b5 & 0x20)
        nes |= kNesBtnB;  // Cross
    if (b5 & 0x40)
        nes |= kNesBtnA;  // Circle
    if (b5 & 0x10)
        nes |= kNesBtnA;  // Square also A
    if (b6 & 0x10)
        nes |= kNesBtnSelect;  // Share
    if (b6 & 0x20)
        nes |= kNesBtnStart;  // Options
    // Left stick
    const int lx = int(data[o]) - 128;
    const int ly = int(data[o + 1]) - 128;
    if (lx > 48)
        nes |= kNesBtnRight;
    if (lx < -48)
        nes |= kNesBtnLeft;
    if (ly < -48)
        nes |= kNesBtnUp;
    if (ly > 48)
        nes |= kNesBtnDown;
    return nes;
}

enum class Layout { Hid, Ds4, Xinput };
struct Report {
    uint8_t nes = 0;
    uint8_t md = 0;
};

inline Report Parse(Layout layout, const uint8_t* data, int length) {
    Report r;
    if (!data || length < 2)
        return r;
    if (layout == Layout::Xinput)
        r.nes = ParseX360Report(data, length);
    else if (layout == Layout::Ds4)
        r.nes = ParseDs4Report(data, length);
    else
        r.nes = ParseGamepadReport(data, length);
    // MD's three-button wire state: Start A B C Right Left Down Up (active high here).
    if (r.nes & kNesBtnUp)
        r.md |= 0x01;
    if (r.nes & kNesBtnDown)
        r.md |= 0x02;
    if (r.nes & kNesBtnLeft)
        r.md |= 0x04;
    if (r.nes & kNesBtnRight)
        r.md |= 0x08;
    if (r.nes & kNesBtnStart)
        r.md |= 0x80;
    uint8_t face = 0;
    if (layout == Layout::Xinput && length >= 10) {
        if (data[3] & 0x40)
            face |= 0x40;  // West -> MD A
        if (data[3] & 0x10)
            face |= 0x20;  // South -> MD B
        if (data[3] & 0x20)
            face |= 0x10;  // East -> MD C
    } else if (layout == Layout::Ds4) {
        const int o = data[0] == 1 ? 1 : 0;
        if (length >= o + 6) {
            if (data[o + 4] & 0x10)
                face |= 0x40;
            if (data[o + 4] & 0x20)
                face |= 0x20;
            if (data[o + 4] & 0x40)
                face |= 0x10;
        }
    } else if (layout == Layout::Hid && length >= 4) {
        int o = 0;
        if (length >= 10 && data[0] == 1 && data[2] <= 0x0f &&
            (data[1] != 0 || data[2] != 0 || data[3] < 16))
            o = 1;
        if (data[o] & 0x04)
            face |= 0x40;
        if (data[o] & 0x01)
            face |= 0x20;
        if (data[o] & 0x02)
            face |= 0x10;
    }
    r.md |= face;
    return r;
}

}  // namespace tab5_gamepad
