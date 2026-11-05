/* SPDX-License-Identifier: MIT */
// hid.hpp - HID report descriptor parser + pointer report decoder (mouse, tablet, touchscreen, touchpad).
// Used by the USB (xHCI) and I2C-HID drivers; both hand raw input reports to PointerDevice::handle().
#pragma once
#include <stdint.h>
#include "touchpad.hpp"

namespace aos::hid {

constexpr int kMaxFields  = 128;
constexpr int kMaxLayouts = 6;
constexpr int kMaxFeat    = 8;
constexpr int kMaxRids    = 12;

struct Field {
    uint16_t bitoff; uint8_t size; uint8_t rid;
    uint16_t page, usage;
    int32_t lmin, lmax, pmin, pmax;
    int8_t uexp; uint8_t unit;
    int8_t finger;               // finger collection index, -1 if none
    uint8_t app;                 // 1 mouse/pointer, 2 touchpad, 3 touchscreen, 4 digitizer/pen
    bool rel;
};

struct FeatField { uint8_t rid; uint16_t bitoff; uint8_t size; uint16_t usage; };
struct RidInfo   { uint8_t rid; uint16_t in_bits, feat_bits; };

struct Layout {
    uint8_t rid = 0;
    uint8_t kind = 0;            // 0 unused, 1 relative mouse, 2 absolute pointer, 3 touchpad, 4 touchscreen
    int16_t x = -1, y = -1, wheel = -1, tip = -1, btn[3] = {-1, -1, -1};
    struct C { int16_t tip, conf, id, x, y; } c[5] = {};
    uint8_t nc = 0;
    int32_t w16 = 0, h16 = 0;    // touchpad size in 1/16 mm
    Touchpad tp;
};

class PointerDevice {
public:
    bool parse(const uint8_t* desc, uint32_t len);      // true if at least one usable pointer report exists
    void use_boot_mouse();                              // fixed 3/4 byte boot-protocol layout
    void set_source(uint8_t src) { src_ = src; }
    void handle(const uint8_t* report, uint32_t len, uint32_t now_ms);
    void tick(uint32_t now_ms);

    bool is_boot() const { return boot_; }
    bool is_touchpad() const;
    // Feature reports that switch a Precision Touchpad out of mouse-emulation mode.
    // build_feature(i): full report incl. leading report-id byte (if ids are used); returns length or 0.
    int  feature_count() const { return nfeat_rids_; }
    int  build_feature(int i, uint8_t* out, uint32_t cap, uint8_t* rid_out) const;
    uint16_t max_input_bytes() const;                   // largest input report incl. id byte

private:
    int32_t get(const uint8_t* r, uint32_t len, const Field& f, bool* ok) const;
    void build_layouts();
    void handle_rel(const Layout& L, const uint8_t* r, uint32_t len);
    void handle_abs(const Layout& L, const uint8_t* r, uint32_t len);
    void handle_touch(Layout& L, const uint8_t* r, uint32_t len, uint32_t now);

    Field fields_[kMaxFields];
    int nfields_ = 0;
    FeatField feat_[kMaxFeat];
    int nfeat_ = 0, nfeat_rids_ = 0;
    uint8_t feat_rids_[kMaxFeat] = {};
    RidInfo rids_[kMaxRids];
    int nrids_ = 0;
    Layout layouts_[kMaxLayouts];
    int nlayouts_ = 0;
    bool has_ids_ = false, boot_ = false;
    uint8_t src_ = 1;
    int rem_x_ = 0, rem_y_ = 0;
};

}  // namespace aos::hid
