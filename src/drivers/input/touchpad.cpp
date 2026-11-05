/* SPDX-License-Identifier: MIT */
#include "touchpad.hpp"
#include "../ps2_mouse.hpp"

namespace aos::hid {
namespace {
bool g_natural = false, g_tap = true, g_two = true, g_seen = false;
int g_speed256 = 256;
inline int iabs(int v) { return v < 0 ? -v : v; }
constexpr uint32_t TAP_MS = 250, DRAG_WINDOW_MS = 250, PULSE_MS = 40;
constexpr int MOVE_START = 16;      // 1 mm: no pointer motion until the finger really moves
constexpr int TAP_FAR = 40;         // 2.5 mm: beyond this it is not a tap
constexpr int SCROLL_STEP = 80;     // 5 mm of two-finger travel = one wheel notch
}

void Touchpad::set_natural_scroll(bool on) { g_natural = on; }
void Touchpad::set_tap_to_click(bool on) { g_tap = on; }
void Touchpad::set_two_finger_scroll(bool on) { g_two = on; }
void Touchpad::set_speed(int percent) { if (percent < 0) percent = 0; if (percent > 100) percent = 100; g_speed256 = 77 + percent * 358 / 100; }
bool Touchpad::seen() { return g_seen; }

// delta: device delta. base256: (pixels per unit) * 256. mag: speed proxy (|dx|+|dy| this report).
int accel_px(int delta, int mag, int base256, int* rem) {
    int extra = mag - 6;
    if (extra < 0) extra = 0;
    if (extra > 48) extra = 48;
    const int gain256 = 256 + extra * 8;                       // 1.0x .. 2.5x
    int v = *rem + delta * base256 * gain256 / 256;          // result in 1/256 px
    int whole = v / 256;
    *rem = v - whole * 256;
    return whole;
}

void Touchpad::init(uint8_t src, int32_t w16, int32_t h16, bool clickpad) {
    *this = Touchpad{};
    g_seen = true;
    src_ = src;
    if (w16 > 0) w_ = w16;
    if (h16 > 0) h_ = h16;
    clickpad_ = clickpad;
}

void Touchpad::click(uint8_t button, uint32_t now) {
    if (pulse_) return;
    pulse_ = true;
    pulse_btn_ = button;
    pulse_end_ = now + PULSE_MS;
}

void Touchpad::flush() {
    const uint8_t b = latch_ | (pulse_ ? pulse_btn_ : 0) | (drag_ ? 1 : 0);
    if (out_dx_ || out_dy_ || out_wheel_ || b != last_buttons_) {
        mouse_push(src_, out_dx_, out_dy_, out_wheel_, b);
        last_buttons_ = b;
    }
    out_dx_ = out_dy_ = out_wheel_ = 0;
}

void Touchpad::tick(uint32_t now) {
    if (pulse_ && static_cast<int32_t>(now - pulse_end_) >= 0) {
        pulse_ = false;
        flush();
    }
    if (have_tap_ && now - last_tap_ > DRAG_WINDOW_MS) have_tap_ = false;
}

void Touchpad::update(const TouchContact* c, int n, bool phys, uint32_t now) {
    bool seen[5] = {false, false, false, false, false};
    const int prev_nf = nf_;

    for (int i = 0; i < n; ++i) {
        if (!c[i].tip) continue;
        int k = -1;
        for (int j = 0; j < 5; ++j) if (s_[j].used && s_[j].id == c[i].id) { k = j; break; }
        if (k < 0) {
            for (int j = 0; j < 5; ++j) if (!s_[j].used) { k = j; break; }
            if (k < 0) continue;
            s_[k] = Slot{true, c[i].id, c[i].x, c[i].y, c[i].x, c[i].y, c[i].x, c[i].y};
        } else {
            s_[k].px = s_[k].x; s_[k].py = s_[k].y;
            s_[k].x = c[i].x;   s_[k].y = c[i].y;
        }
        seen[k] = true;
    }
    nf_ = 0;
    for (int j = 0; j < 5; ++j) {
        if (s_[j].used && !seen[j]) s_[j].used = false;
        if (s_[j].used) ++nf_;
    }

    if (prev_nf == 0 && nf_ > 0) {                          // new touch sequence
        touch_start_ = now; max_f_ = nf_; far_ = false; started_ = false;
        scroll_acc_ = 0; btn_used_ = false; rem_x_ = rem_y_ = 0;
        drag_cand_ = have_tap_ && nf_ == 1 && (now - last_tap_) <= DRAG_WINDOW_MS;
    }
    if (nf_ > max_f_) max_f_ = nf_;

    int first = -1;
    for (int j = 0; j < 5; ++j) if (s_[j].used) { first = j; break; }

    // ---- physical (clickpad) button
    if (phys && !phys_prev_) {
        uint8_t b = 1;
        if (nf_ >= 3) b = 4;
        else if (nf_ == 2) b = 2;
        else if (nf_ == 1 && clickpad_ && s_[first].x > w_ * 62 / 100 && s_[first].y > h_ * 75 / 100) b = 2;
        latch_ = b;
        btn_used_ = true;
    } else if (!phys && phys_prev_) {
        latch_ = 0;
    }
    phys_prev_ = phys;

    // ---- how far did any finger travel since touchdown?
    for (int j = 0; j < 5; ++j)
        if (s_[j].used && iabs(s_[j].x - s_[j].sx) + iabs(s_[j].y - s_[j].sy) > TAP_FAR) far_ = true;

    // ---- motion / scroll
    if (nf_ == 1 && max_f_ == 1) {
        const Slot& s = s_[first];
        const int dx = s.x - s.px, dy = s.y - s.py;
        const int disp = iabs(s.x - s.sx) + iabs(s.y - s.sy);
        if (!started_ && (disp >= MOVE_START || latch_)) {
            started_ = true;
            if (drag_cand_ && !drag_) drag_ = true;         // tap, then touch + move = drag (button held)
        }
        if (started_ && (dx || dy)) {
            const int mag = iabs(dx) + iabs(dy);
            out_dx_ += accel_px(dx, mag, 80 * g_speed256 / 256, &rem_x_);       // 80/256 px per 1/16 mm = 5 px/mm
            out_dy_ += accel_px(dy, mag, 80 * g_speed256 / 256, &rem_y_);
        }
    } else if (nf_ == 2 && g_two) {
        int sum = 0, cnt = 0;
        for (int j = 0; j < 5; ++j)
            if (s_[j].used) { sum += s_[j].y - s_[j].py; ++cnt; }
        if (cnt) scroll_acc_ += sum / cnt;
        // fingers moving up (dy < 0) scroll the page down (+wheel) unless natural scrolling is on
        while (scroll_acc_ >= SCROLL_STEP)  { out_wheel_ += g_natural ? 1 : -1;  scroll_acc_ -= SCROLL_STEP; }
        while (scroll_acc_ <= -SCROLL_STEP) { out_wheel_ += g_natural ? -1 : 1;  scroll_acc_ += SCROLL_STEP; }
    }

    // ---- all fingers lifted
    if (prev_nf > 0 && nf_ == 0) {
        if (drag_) {
            drag_ = false;
        } else if (g_tap && !far_ && !btn_used_ && now - touch_start_ < TAP_MS && max_f_ <= 3) {
            click(max_f_ == 1 ? 1 : (max_f_ == 2 ? 2 : 4), now);
            if (max_f_ == 1) { last_tap_ = now; have_tap_ = true; }
        } else {
            have_tap_ = false;
        }
        started_ = false;
    }

    flush();
}

}  // namespace aos::hid
