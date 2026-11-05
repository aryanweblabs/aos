/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

struct MouseState {
    int     x;
    int     y;
    uint8_t buttons;   // bit0=left, bit1=right, bit2=middle
    bool    moved;
    int     wheel;     // scroll notches this poll (+down / -up), 0 if none
};

void mouse_init(int screen_width, int screen_height);
void mouse_irq_byte(uint8_t value);                      // IRQ12 only
bool mouse_pending();
MouseState mouse_poll(int screen_width, int screen_height);

// Keyboard-driven pointer: move by (dx,dy) (dy>0 = up); button: -1 unchanged, 0 left up, 1 left down.
void mouse_inject(int dx, int dy, int button);
void mouse_set_speed(int percent);   // 0..100 (22 = 0.5x = default, 50 = ~1.0x, 100 = ~1.7x), PS/2 and USB mice
int  mouse_get_speed();
void mouse_set_swap(bool on);        // swap left/right buttons
int  mouse_scale_x(int v);
int  mouse_scale_y(int v);

// ---- Pointer sources other than the PS/2 port (USB HID, I2C-HID touchpads) ----
// src = 1..7 (each source keeps its own button state; the final state is the OR of all of them).
// dy_down > 0 moves the pointer DOWN (HID convention). wheel: +down / -up.
void mouse_push(uint8_t src, int dx, int dy_down, int wheel, uint8_t buttons);
// Absolute position, nx/ny in 0..65535 (tablets, touchscreens).
void mouse_push_abs(uint8_t src, int nx, int ny, int wheel, uint8_t buttons);
