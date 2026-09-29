#pragma once

#include <cstdint>

// NES joypad bitmask used by nofrendo / FcEmulatorService.
// bit0 A, bit1 B, bit2 Select, bit3 Start, bit4 Up, bit5 Down, bit6 Left, bit7 Right.
enum : uint8_t {
    kNesBtnA = 0x01,
    kNesBtnB = 0x02,
    kNesBtnSelect = 0x04,
    kNesBtnStart = 0x08,
    kNesBtnUp = 0x10,
    kNesBtnDown = 0x20,
    kNesBtnLeft = 0x40,
    kNesBtnRight = 0x80,
};

// Start USB host + HID client. Safe to call once at boot. Returns false if USB
// host cannot start (device still usable without a pad).
bool UsbGamepadHostStart();

// Optional: called when a gamepad connects so the board can free internal RAM
// (USB endpoints cannot allocate when SRAM is exhausted).
void UsbGamepadSetOnConnect(void (*cb)());

// Live NES bitmask from the first connected gamepad (0 if none).
uint8_t UsbGamepadNesMask();

// True when a HID gamepad is currently connected.
bool UsbGamepadConnected();
