/* SPDX-License-Identifier: MIT */
// PS/2 mouse, interrupt driven. Supports Intellimouse (wheel) when available.
// Built with -mgeneral-regs-only because mouse_irq_byte runs in IRQ context.
#include "ps2_mouse.hpp"

namespace {
inline void out8(uint16_t port, uint8_t v) { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
inline uint8_t in8(uint16_t port) { uint8_t v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
bool wait_input()  { for (unsigned i = 0; i < 100000; ++i) if (!(in8(0x64) & 2)) return true; return false; }
bool wait_output() { for (unsigned i = 0; i < 100000; ++i) if (in8(0x64) & 1) return true; return false; }
void command(uint8_t c) { wait_input(); out8(0x64, c); }
void data(uint8_t d)    { wait_input(); out8(0x60, d); }
uint8_t read_data()     { if (!wait_output()) return 0; return in8(0x60); }
void mouse_command(uint8_t value) { command(0xD4); data(value); }

int mouse_x = 0, mouse_y = 0;
uint8_t last_buttons = 0;

volatile int acc_dx = 0, acc_dy = 0, acc_wheel = 0;
volatile uint8_t buttons = 0;
volatile bool dirty = false;
volatile uint8_t src_buttons[8] = {0};
int g_sw = 0, g_sh = 0;
volatile bool abs_pending = false;
volatile int abs_x = 0, abs_y = 0;

uint8_t packet[4];
uint8_t packet_index = 0;
uint8_t packet_size = 3;       // 3 = basic, 4 = Intellimouse
bool enabled = false;

// Pointer acceleration (integer only): slow moves stay 1:1 for pixel-exact aiming, fast flicks speed up to ~3x.
// mag = |dx|+|dy| of the packet; the remainder is carried so nothing is lost to rounding.
int g_arem_x = 0, g_arem_y = 0;
inline int iabs_(int v) { return v < 0 ? -v : v; }
inline int accel_gain(int mag) {            // 8.8 fixed point
    if (mag <= 5) return 256;                       // slow / precise moves stay 1:1
    const int e = mag - 5 > 40 ? 40 : mag - 5;
    return 256 + e * 6;                             // fast flicks: up to ~2x of the base speed
}
inline int accel_axis(int v, int gain, int* rem) {
    const int t = *rem + v * gain;
    const int w = t / 256;
    *rem = t - w * 256;
    return w;
}

inline uint64_t irq_save() {
    uint64_t f;
    asm volatile("pushfq; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
inline void irq_restore(uint64_t f) {
    if (f & 0x200) asm volatile("sti" ::: "memory");
}
}  // namespace

void mouse_init(int screen_width, int screen_height) {
    for (unsigned i = 0; i < 16 && (in8(0x64) & 1); ++i) (void)in8(0x60);

    command(0xAE);                          // enable keyboard port
    command(0xA8);                          // enable auxiliary device
    command(0x20);
    uint8_t status = read_data();
    status |= 0x43;                         // IRQ1 + IRQ12 enabled, scancode translation (set 1)
    status &= ~0x30;                        // clear 'keyboard disabled' / 'mouse disabled'
    command(0x60);
    data(status);
    data(0xF4); read_data();                // keyboard: enable scanning (ACK)
    for (unsigned i = 0; i < 16 && (in8(0x64) & 1); ++i) (void)in8(0x60);

    mouse_command(0xF6); read_data();       // set defaults

    // Try Intellimouse (wheel) magic sequence
    mouse_command(0xF3); read_data(); data(200); read_data();
    mouse_command(0xF3); read_data(); data(100); read_data();
    mouse_command(0xF3); read_data(); data(80);  read_data();
    mouse_command(0xF2);                    // Get device ID
    read_data();                            // ACK
    const uint8_t id = read_data();
    packet_size = (id == 0x03 || id == 0x04) ? 4 : 3;

    g_sw = screen_width; g_sh = screen_height;
    mouse_x = screen_width  > 0 ? screen_width  / 2 : 0;
    mouse_y = screen_height > 0 ? screen_height / 2 : 0;
    packet_index = 0;
    buttons = 0; last_buttons = 0;
    acc_dx = acc_dy = acc_wheel = 0;
    dirty = false;

    mouse_command(0xF4);                    // enable streaming
    enabled = (read_data() == 0xFA);
}

void mouse_irq_byte(uint8_t value) {
    if (!enabled) return;
    if (packet_index == 0 && !(value & 0x08)) return;   // wait for valid header
    packet[packet_index++] = value;
    if (packet_index < packet_size) return;
    packet_index = 0;

    int dx = static_cast<int8_t>(packet[1]);
    int dy = static_cast<int8_t>(packet[2]);
    if (packet[0] & 0x40) dx = (packet[0] & 0x10) ? -255 : 255;
    if (packet[0] & 0x80) dy = (packet[0] & 0x20) ? -255 : 255;

    const int gain = accel_gain(iabs_(dx) + iabs_(dy));
    acc_dx += mouse_scale_x(accel_axis(dx, gain, &g_arem_x));
    acc_dy += mouse_scale_y(accel_axis(dy, gain, &g_arem_y));
    buttons = packet[0] & 7;

    if (packet_size == 4) {
        // wheel is low 4 bits of 4th byte, signed
        int8_t w = static_cast<int8_t>(packet[3] & 0x0F);
        if (w & 0x08) w |= static_cast<int8_t>(0xF0);  // sign-extend 4-bit
        acc_wheel += w;
    }
    dirty = true;
}

bool mouse_pending() { return dirty; }

namespace { int g_speed256 = 128, g_rem_x = 0, g_rem_y = 0; bool g_swap = false; }
void mouse_set_speed(int percent) { if (percent < 0) percent = 0; if (percent > 100) percent = 100; g_speed256 = 40 + percent * 4; }   // 22% = 0.5x (default), 50% = ~1.0x, 100% = ~1.7x
int mouse_get_speed() { return (g_speed256 - 40) / 4; }
void mouse_set_swap(bool on) { g_swap = on; }
int mouse_scale_x(int v) { const int t = g_rem_x + v * g_speed256; const int w = t / 256; g_rem_x = t - w * 256; return w; }
int mouse_scale_y(int v) { const int t = g_rem_y + v * g_speed256; const int w = t / 256; g_rem_y = t - w * 256; return w; }

MouseState mouse_poll(int screen_width, int screen_height) {
    const uint64_t f = irq_save();
    const int dx = acc_dx, dy = acc_dy, wh = acc_wheel;
    uint8_t b = buttons;
    for (int i = 1; i < 8; ++i) b |= src_buttons[i];
    if (g_swap) b = static_cast<uint8_t>((b & ~3) | ((b & 1) << 1) | ((b & 2) >> 1));
    const bool had = dirty;
    const bool have_abs = abs_pending;
    const int ax = abs_x, ay = abs_y;
    abs_pending = false;
    acc_dx = acc_dy = acc_wheel = 0;
    dirty = false;
    irq_restore(f);

    if (have_abs) {
        mouse_x = static_cast<int>((static_cast<int64_t>(ax) * (g_sw > 1 ? g_sw - 1 : 0)) / 65535);
        mouse_y = static_cast<int>((static_cast<int64_t>(ay) * (g_sh > 1 ? g_sh - 1 : 0)) / 65535);
    }

    mouse_x += dx;
    mouse_y -= dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (screen_width  > 0 && mouse_x >= screen_width)  mouse_x = screen_width  - 1;
    if (screen_height > 0 && mouse_y >= screen_height) mouse_y = screen_height - 1;
    last_buttons = b;
    return {mouse_x, mouse_y, b, had, wh};
}

void mouse_inject(int dx, int dy, int button) {
    acc_dx += dx;
    acc_dy += dy;
    if (button == 1) buttons = buttons | 1;
    else if (button == 0) buttons = buttons & ~1;
    dirty = true;
}

void mouse_push(uint8_t src, int dx, int dy_down, int wheel, uint8_t btn) {
    if (src == 0 || src > 7) return;
    const uint64_t f = irq_save();
    acc_dx += dx;
    acc_dy -= dy_down;          // ps/2 convention: dy > 0 = up (mouse_poll subtracts it)
    acc_wheel += wheel;
    src_buttons[src] = btn & 7;
    dirty = true;
    irq_restore(f);
}

void mouse_push_abs(uint8_t src, int nx, int ny, int wheel, uint8_t btn) {
    if (src == 0 || src > 7) return;
    nx = nx < 0 ? 0 : (nx > 65535 ? 65535 : nx);
    ny = ny < 0 ? 0 : (ny > 65535 ? 65535 : ny);
    const uint64_t f = irq_save();
    abs_x = nx; abs_y = ny; abs_pending = true;
    acc_wheel += wheel;
    src_buttons[src] = btn & 7;
    dirty = true;
    irq_restore(f);
}
