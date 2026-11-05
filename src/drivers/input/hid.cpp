/* SPDX-License-Identifier: MIT */
#include "hid.hpp"
#include "../ps2_mouse.hpp"

namespace aos::hid {
namespace {

constexpr uint16_t PG_DESKTOP = 0x01, PG_BUTTON = 0x09, PG_DIGITIZER = 0x0D;

struct Glob {
    uint16_t page = 0;
    int32_t lmin = 0, lmax = 0, pmin = 0, pmax = 0;
    int8_t uexp = 0; uint8_t unit = 0;
    uint32_t rsize = 0, rcount = 0;
    uint8_t rid = 0;
};
struct Col { uint16_t page, usage; int8_t finger; };

inline int iabs(int v) { return v < 0 ? -v : v; }

// physical extent in 1/256 mm, 0 if unknown
int64_t extent256(int32_t pmin, int32_t pmax, int8_t uexp, uint8_t unit) {
    int64_t v = static_cast<int64_t>(pmax) - pmin;
    if (v <= 0) return 0;
    int64_t f;
    if (unit == 0x11) f = 2560;            // cm
    else if (unit == 0x13) f = 6502;       // inch
    else return 0;
    v *= f;
    for (int e = uexp; e > 0; --e) v *= 10;
    for (int e = uexp; e < 0; ++e) v /= 10;
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
bool PointerDevice::parse(const uint8_t* d, uint32_t n) {
    nfields_ = nfeat_ = nfeat_rids_ = nrids_ = nlayouts_ = 0;
    has_ids_ = boot_ = false;
    rem_x_ = rem_y_ = 0;

    Glob g, stack[4];
    int sp = 0;
    uint32_t uses[48];
    int nu = 0;
    bool hasrange = false;
    uint32_t umin = 0, umax = 0;
    Col cols[12];
    int ncols = 0, finger_ctr = 0;
    uint8_t app = 0;

    auto rid_info = [&](uint8_t rid) -> RidInfo* {
        for (int i = 0; i < nrids_; ++i) if (rids_[i].rid == rid) return &rids_[i];
        if (nrids_ >= kMaxRids) return nullptr;
        rids_[nrids_] = RidInfo{rid, 0, 0};
        return &rids_[nrids_++];
    };
    auto cur_finger = [&]() -> int8_t {
        for (int i = ncols - 1; i >= 0; --i) if (cols[i].finger >= 0) return cols[i].finger;
        return -1;
    };

    uint32_t i = 0;
    while (i < n) {
        const uint8_t b = d[i++];
        if (b == 0xFE) { if (i >= n) break; i += 2u + d[i]; continue; }
        uint32_t sz = b & 3; if (sz == 3) sz = 4;
        const uint8_t type = (b >> 2) & 3, tag = b >> 4;
        if (i + sz > n) break;
        uint32_t u = 0;
        for (uint32_t k = 0; k < sz; ++k) u |= static_cast<uint32_t>(d[i + k]) << (8 * k);
        int32_t s = static_cast<int32_t>(u);
        if (sz == 1) s = static_cast<int8_t>(u);
        else if (sz == 2) s = static_cast<int16_t>(u);
        i += sz;

        if (type == 1) {                                       // global
            switch (tag) {
                case 0: g.page = static_cast<uint16_t>(u); break;
                case 1: g.lmin = s; break;
                case 2: g.lmax = s;
                        // an unsigned field whose top bit is set (e.g. 0xFF with min 0) must not turn negative
                        if (g.lmin >= 0 && s < 0) g.lmax = static_cast<int32_t>(sz == 1 ? (u & 0xFF) : sz == 2 ? (u & 0xFFFF) : u);
                        break;
                case 3: g.pmin = s; break;
                case 4: g.pmax = s; break;
                case 5: g.uexp = static_cast<int8_t>(u > 7 ? static_cast<int>(u & 0xF) - 16 : static_cast<int>(u & 0xF)); break;
                case 6: g.unit = static_cast<uint8_t>(u); break;
                case 7: g.rsize = u; break;
                case 8: g.rid = static_cast<uint8_t>(u); has_ids_ = true; break;
                case 9: g.rcount = u; break;
                case 10: if (sp < 4) stack[sp++] = g; break;
                case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {                                // local
            if (tag == 0) { if (nu < 48) uses[nu++] = (sz == 4) ? u : (u & 0xFFFF); }
            else if (tag == 1) { umin = u; hasrange = true; }
            else if (tag == 2) { umax = u; hasrange = true; }
        } else if (type == 0) {                                // main
            if (tag == 10) {                                   // collection
                const uint32_t us = nu ? uses[0] : 0;
                const uint16_t pg = (us >> 16) ? static_cast<uint16_t>(us >> 16) : g.page;
                const uint16_t uc = static_cast<uint16_t>(us & 0xFFFF);
                int8_t fi = -1;
                if (pg == PG_DIGITIZER && uc == 0x22) fi = static_cast<int8_t>(finger_ctr++);
                if ((u & 0xFF) == 1) {                         // application
                    finger_ctr = 0;
                    app = 0;
                    if (pg == PG_DESKTOP && (uc == 2 || uc == 1)) app = 1;
                    else if (pg == PG_DIGITIZER && uc == 5) app = 2;
                    else if (pg == PG_DIGITIZER && uc == 4) app = 3;
                    else if (pg == PG_DIGITIZER && (uc == 1 || uc == 2 || uc == 3)) app = 4;
                }
                if (ncols < 12) cols[ncols++] = Col{pg, uc, fi};
            } else if (tag == 12) {                            // end collection
                if (ncols > 0) --ncols;
                if (ncols == 0) app = 0;
            } else if (tag == 8 || tag == 11) {                // input / feature
                RidInfo* ri = rid_info(g.rid);
                const bool cst = u & 1, var = u & 2, rel = u & 4;
                for (uint32_t k = 0; k < g.rcount && ri; ++k) {
                    uint32_t us;
                    if (static_cast<int>(k) < nu) us = uses[k];
                    else if (hasrange) us = umin + k > umax ? umax : umin + k;
                    else if (nu) us = uses[nu - 1];
                    else us = 0;
                    const uint16_t pg = (us >> 16) ? static_cast<uint16_t>(us >> 16) : g.page;
                    const uint16_t uc = static_cast<uint16_t>(us & 0xFFFF);
                    if (tag == 8) {
                        if (!cst && var && g.rsize <= 32 && g.rsize > 0 && nfields_ < kMaxFields && (pg || uc)) {
                            Field& f = fields_[nfields_++];
                            f.bitoff = ri->in_bits; f.size = static_cast<uint8_t>(g.rsize); f.rid = g.rid;
                            f.page = pg; f.usage = uc;
                            f.lmin = g.lmin; f.lmax = g.lmax; f.pmin = g.pmin; f.pmax = g.pmax;
                            f.uexp = g.uexp; f.unit = g.unit;
                            f.finger = cur_finger(); f.app = app; f.rel = rel;
                        }
                        ri->in_bits = static_cast<uint16_t>(ri->in_bits + g.rsize);
                    } else {
                        if (!cst && var && pg == PG_DIGITIZER && (uc == 0x52 || uc == 0x57 || uc == 0x58) &&
                            nfeat_ < kMaxFeat)
                            feat_[nfeat_++] = FeatField{g.rid, ri->feat_bits, static_cast<uint8_t>(g.rsize), uc};
                        ri->feat_bits = static_cast<uint16_t>(ri->feat_bits + g.rsize);
                    }
                }
            }
            nu = 0; hasrange = false; umin = umax = 0;
        }
    }

    for (int k = 0; k < nfeat_; ++k) {
        bool seen = false;
        for (int j = 0; j < nfeat_rids_; ++j) if (feat_rids_[j] == feat_[k].rid) seen = true;
        if (!seen) feat_rids_[nfeat_rids_++] = feat_[k].rid;
    }
    build_layouts();
    return nlayouts_ > 0;
}

void PointerDevice::build_layouts() {
    nlayouts_ = 0;
    for (int ri = 0; ri < nrids_ && nlayouts_ < kMaxLayouts; ++ri) {
        Layout& L = layouts_[nlayouts_];
        L = Layout{};
        L.rid = rids_[ri].rid;
        int fingers_seen[5]; int nseen = 0;
        bool rel = false; uint8_t app = 0;
        int cx = -1;                                          // field index of contact 0 X (for ranges)
        for (int k = 0; k < nfields_; ++k) {
            const Field& f = fields_[k];
            if (f.rid != L.rid) continue;
            if (f.app) app = f.app;
            if (f.finger < 0) {
                if (f.page == PG_BUTTON && f.usage >= 1 && f.usage <= 3) L.btn[f.usage - 1] = static_cast<int16_t>(k);
                else if (f.page == PG_DESKTOP && f.usage == 0x30) { L.x = static_cast<int16_t>(k); rel = f.rel; }
                else if (f.page == PG_DESKTOP && f.usage == 0x31) L.y = static_cast<int16_t>(k);
                else if (f.page == PG_DESKTOP && f.usage == 0x38) L.wheel = static_cast<int16_t>(k);
                else if (f.page == PG_DIGITIZER && f.usage == 0x42) L.tip = static_cast<int16_t>(k);
                continue;
            }
            int slot = -1;
            for (int j = 0; j < nseen; ++j) if (fingers_seen[j] == f.finger) slot = j;
            if (slot < 0) {
                if (nseen >= 5) continue;
                slot = nseen; fingers_seen[nseen++] = f.finger;
                L.c[slot] = Layout::C{-1, -1, -1, -1, -1};
            }
            Layout::C& c = L.c[slot];
            if (f.page == PG_DIGITIZER && f.usage == 0x42) c.tip = static_cast<int16_t>(k);
            else if (f.page == PG_DIGITIZER && f.usage == 0x47) c.conf = static_cast<int16_t>(k);
            else if (f.page == PG_DIGITIZER && f.usage == 0x51) c.id = static_cast<int16_t>(k);
            else if (f.page == PG_DESKTOP && f.usage == 0x30) { c.x = static_cast<int16_t>(k); if (slot == 0) cx = k; }
            else if (f.page == PG_DESKTOP && f.usage == 0x31) c.y = static_cast<int16_t>(k);
        }
        L.nc = static_cast<uint8_t>(nseen);
        if (nseen > 0 && L.c[0].x >= 0 && L.c[0].y >= 0) {
            L.kind = (app == 3) ? 4 : 3;
            const Field& fx = fields_[L.c[0].x];
            const Field& fy = fields_[L.c[0].y];
            int64_t wm = extent256(fx.pmin, fx.pmax, fx.uexp, fx.unit);   // 1/256 mm
            int64_t hm = extent256(fy.pmin, fy.pmax, fy.uexp, fy.unit);
            const int64_t xr = static_cast<int64_t>(fx.lmax) - fx.lmin, yr = static_cast<int64_t>(fy.lmax) - fy.lmin;
            if (wm <= 0 || hm <= 0) {                                      // no physical info: assume 90 mm wide
                wm = 90 * 256;
                hm = xr > 0 ? wm * yr / xr : 60 * 256;
            }
            L.w16 = static_cast<int32_t>(wm / 16);
            L.h16 = static_cast<int32_t>(hm / 16);
            if (L.kind == 3) L.tp.init(src_, L.w16, L.h16, L.btn[0] >= 0);
        } else if (L.x >= 0 && L.y >= 0) {
            L.kind = rel ? 1 : 2;
        }
        (void)cx;
        if (L.kind) ++nlayouts_;
    }
}

bool PointerDevice::is_touchpad() const {
    for (int i = 0; i < nlayouts_; ++i) if (layouts_[i].kind == 3) return true;
    return false;
}

uint16_t PointerDevice::max_input_bytes() const {
    uint16_t m = 0;
    for (int i = 0; i < nrids_; ++i) {
        const uint16_t b = static_cast<uint16_t>((rids_[i].in_bits + 7) / 8 + (has_ids_ ? 1 : 0));
        if (b > m) m = b;
    }
    return m;
}

int PointerDevice::build_feature(int idx, uint8_t* out, uint32_t cap, uint8_t* rid_out) const {
    if (idx < 0 || idx >= nfeat_rids_) return 0;
    const uint8_t rid = feat_rids_[idx];
    uint16_t bits = 0;
    for (int i = 0; i < nrids_; ++i) if (rids_[i].rid == rid) bits = rids_[i].feat_bits;
    const uint32_t bytes = (bits + 7u) / 8u, off = has_ids_ ? 1u : 0u;
    if (bytes + off > cap) return 0;
    for (uint32_t i = 0; i < bytes + off; ++i) out[i] = 0;
    if (has_ids_) out[0] = rid;
    for (int k = 0; k < nfeat_; ++k) {
        if (feat_[k].rid != rid) continue;
        const uint32_t v = feat_[k].usage == 0x52 ? 3u : 1u;       // input mode = PTP, switches = on
        for (uint32_t b = 0; b < feat_[k].size && b < 32; ++b) {
            if (!((v >> b) & 1)) continue;
            const uint32_t bit = feat_[k].bitoff + b;
            out[off + (bit >> 3)] |= static_cast<uint8_t>(1u << (bit & 7));
        }
    }
    if (rid_out) *rid_out = rid;
    return static_cast<int>(bytes + off);
}

void PointerDevice::use_boot_mouse() {
    boot_ = true;
    nlayouts_ = 0;
    rem_x_ = rem_y_ = 0;
}

// ---------------------------------------------------------------------------------------------
int32_t PointerDevice::get(const uint8_t* r, uint32_t len, const Field& f, bool* ok) const {
    const uint32_t bit0 = f.bitoff + (has_ids_ ? 8u : 0u);
    if ((bit0 + f.size + 7) / 8 > len) { if (ok) *ok = false; return 0; }
    uint32_t v = 0;
    for (uint32_t b = 0; b < f.size; ++b) {
        const uint32_t bit = bit0 + b;
        if ((r[bit >> 3] >> (bit & 7)) & 1) v |= 1u << b;
    }
    if (f.lmin < 0 && f.size < 32 && (v & (1u << (f.size - 1)))) v |= ~0u << f.size;
    return static_cast<int32_t>(v);
}

void PointerDevice::handle(const uint8_t* r, uint32_t len, uint32_t now) {
    if (boot_) {
        if (len < 3) return;
        const int dx = static_cast<int8_t>(r[1]), dy = static_cast<int8_t>(r[2]);
        const int wh = len >= 4 ? -static_cast<int8_t>(r[3]) : 0;
        const int mag = iabs(dx) + iabs(dy);
        const int ax = mouse_scale_x(accel_px(dx, mag, 256 * 1, &rem_x_));
        const int ay = mouse_scale_y(accel_px(dy, mag, 256 * 1, &rem_y_));
        mouse_push(src_, ax, ay, wh, r[0] & 7);
        return;
    }
    uint8_t rid = 0;
    if (has_ids_) { if (!len) return; rid = r[0]; }
    for (int i = 0; i < nlayouts_; ++i) {
        Layout& L = layouts_[i];
        if (L.rid != rid) continue;
        if (L.kind == 1) handle_rel(L, r, len);
        else if (L.kind == 2) handle_abs(L, r, len);
        else if (L.kind == 3 || L.kind == 4) handle_touch(L, r, len, now);
        return;
    }
}

void PointerDevice::tick(uint32_t now) {
    for (int i = 0; i < nlayouts_; ++i) if (layouts_[i].kind == 3) layouts_[i].tp.tick(now);
}

void PointerDevice::handle_rel(const Layout& L, const uint8_t* r, uint32_t len) {
    bool ok = true;
    const int dx = get(r, len, fields_[L.x], &ok), dy = get(r, len, fields_[L.y], &ok);
    const int wh = L.wheel >= 0 ? -get(r, len, fields_[L.wheel], &ok) : 0;
    uint8_t b = 0;
    for (int k = 0; k < 3; ++k) if (L.btn[k] >= 0 && get(r, len, fields_[L.btn[k]], &ok)) b |= static_cast<uint8_t>(1 << k);
    if (!ok) return;
    const int mag = iabs(dx) + iabs(dy);
    const int ax = mouse_scale_x(accel_px(dx, mag, 256, &rem_x_));
    const int ay = mouse_scale_y(accel_px(dy, mag, 256, &rem_y_));
    mouse_push(src_, ax, ay, wh, b);
}

void PointerDevice::handle_abs(const Layout& L, const uint8_t* r, uint32_t len) {
    bool ok = true;
    const Field& fx = fields_[L.x]; const Field& fy = fields_[L.y];
    const int64_t x = get(r, len, fx, &ok), y = get(r, len, fy, &ok);
    const int wh = L.wheel >= 0 ? -get(r, len, fields_[L.wheel], &ok) : 0;
    uint8_t b = 0;
    for (int k = 0; k < 3; ++k) if (L.btn[k] >= 0 && get(r, len, fields_[L.btn[k]], &ok)) b |= static_cast<uint8_t>(1 << k);
    if (L.tip >= 0 && get(r, len, fields_[L.tip], &ok)) b |= 1;
    if (!ok) return;
    const int64_t xr = static_cast<int64_t>(fx.lmax) - fx.lmin, yr = static_cast<int64_t>(fy.lmax) - fy.lmin;
    if (xr <= 0 || yr <= 0) return;
    mouse_push_abs(src_, static_cast<int>((x - fx.lmin) * 65535 / xr), static_cast<int>((y - fy.lmin) * 65535 / yr), wh, b);
}

void PointerDevice::handle_touch(Layout& L, const uint8_t* r, uint32_t len, uint32_t now) {
    bool ok = true;
    TouchContact tc[5];
    int n = 0;
    const Field& fx = fields_[L.c[0].x]; const Field& fy = fields_[L.c[0].y];
    const int64_t xr = static_cast<int64_t>(fx.lmax) - fx.lmin, yr = static_cast<int64_t>(fy.lmax) - fy.lmin;
    if (xr <= 0 || yr <= 0) return;
    for (int i = 0; i < L.nc; ++i) {
        const Layout::C& c = L.c[i];
        if (c.x < 0 || c.y < 0) continue;
        const bool tip = c.tip >= 0 ? get(r, len, fields_[c.tip], &ok) != 0 : true;
        const bool conf = c.conf >= 0 ? get(r, len, fields_[c.conf], &ok) != 0 : true;   // palm = confidence 0
        const int id = c.id >= 0 ? get(r, len, fields_[c.id], &ok) : i;
        const int64_t x = get(r, len, fields_[c.x], &ok), y = get(r, len, fields_[c.y], &ok);
        if (!ok) return;
        tc[n].tip = tip && conf;
        tc[n].id = static_cast<uint8_t>(id);
        tc[n].x = static_cast<int32_t>((x - fx.lmin) * L.w16 / xr);
        tc[n].y = static_cast<int32_t>((y - fy.lmin) * L.h16 / yr);
        ++n;
    }
    bool phys = false;
    if (L.btn[0] >= 0) phys = get(r, len, fields_[L.btn[0]], &ok) != 0;
    if (!ok) return;

    if (L.kind == 4) {                                           // touchscreen: absolute, first finger = left button
        for (int i = 0; i < n; ++i) {
            if (!tc[i].tip) continue;
            mouse_push_abs(src_, static_cast<int>(static_cast<int64_t>(tc[i].x) * 65535 / (L.w16 ? L.w16 : 1)),
                           static_cast<int>(static_cast<int64_t>(tc[i].y) * 65535 / (L.h16 ? L.h16 : 1)), 0, 1);
            return;
        }
        mouse_push(src_, 0, 0, 0, 0);
        return;
    }
    L.tp.update(tc, n, phys, now);
}

}  // namespace aos::hid
