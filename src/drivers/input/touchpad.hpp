/* SPDX-License-Identifier: MIT */
// touchpad.hpp - multi-touch gesture engine (tap, two-finger tap, tap-and-drag, two-finger scroll,
// clickpad button zones, pointer acceleration). Input is in 1/16 mm units from the pad's top-left corner.
#pragma once
#include <stdint.h>

namespace aos::hid {

struct TouchContact { bool tip; uint8_t id; int32_t x, y; };

class Touchpad {
public:
    void init(uint8_t src, int32_t width16, int32_t height16, bool clickpad);
    void update(const TouchContact* c, int n, bool physical_button, uint32_t now_ms);
    void tick(uint32_t now_ms);                    // releases click pulses; call often
    static void set_natural_scroll(bool on);
    static void set_tap_to_click(bool on);
    static void set_two_finger_scroll(bool on);
    static void set_speed(int percent);            // 0..100, 50 = default
    static bool seen();

private:
    struct Slot { bool used; uint8_t id; int32_t x, y, px, py, sx, sy; };
    void flush();
    void click(uint8_t button, uint32_t now);

    uint8_t  src_ = 1;
    int32_t  w_ = 1440, h_ = 960;                  // 90 x 60 mm
    bool     clickpad_ = false;
    Slot     s_[5] = {};
    int      nf_ = 0, max_f_ = 0;
    uint32_t touch_start_ = 0, last_tap_ = 0, pulse_end_ = 0;
    bool     have_tap_ = false, far_ = false, started_ = false, drag_cand_ = false, drag_ = false;
    bool     btn_used_ = false, phys_prev_ = false, pulse_ = false;
    uint8_t  latch_ = 0, pulse_btn_ = 0, last_buttons_ = 0;
    int      scroll_acc_ = 0, rem_x_ = 0, rem_y_ = 0;
    int      out_dx_ = 0, out_dy_ = 0, out_wheel_ = 0;
};

// Pointer acceleration helper shared with plain HID mice. Returns whole pixels, keeps the remainder.
int accel_px(int delta, int mag, int base256, int* rem);

}  // namespace aos::hid
