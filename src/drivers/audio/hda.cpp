/* SPDX-License-Identifier: MIT */
// hda.cpp - Intel HDA driver. Command path: CORB/RIRB. Output: one 48 kHz / 16-bit / stereo stream, BDL with 2 entries.
// Every output pin (speaker, headphone, line-out) is wired to a DAC and powered; volume/mute act on the amps on that path.
#include "hda.hpp"
#include "../pciutil.hpp"
#include "../timeutil.hpp"
#include "../../memory/physical/pmm.h"
#include "../../memory/virtual/vmm.hpp"

extern "C" {
void aos_serial_write(const char* text);
void* memset(void* s, int c, unsigned long n);
}

namespace aos::audio {
namespace {

void log(const char* s) { aos_serial_write("[hda] "); aos_serial_write(s); aos_serial_write("\r\n"); }

enum : uint32_t { GCAP = 0x00, GCTL = 0x08, STATESTS = 0x0E, CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4A, CORBCTL = 0x4C, CORBSIZE = 0x4E,
                  RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5A, RIRBCTL = 0x5C, RIRBSTS = 0x5D, RIRBSIZE = 0x5E };
enum : uint32_t { SD_CTL = 0x00, SD_STS = 0x03, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FMT = 0x12, SD_BDLPL = 0x18, SD_BDLPU = 0x1C };

uint64_t g_bar = 0;
inline uint8_t  rd8(uint32_t o)  { return *reinterpret_cast<volatile uint8_t*>(g_bar + o); }
inline uint16_t rd16(uint32_t o) { return *reinterpret_cast<volatile uint16_t*>(g_bar + o); }
inline uint32_t rd32(uint32_t o) { return *reinterpret_cast<volatile uint32_t*>(g_bar + o); }
inline void wr8(uint32_t o, uint8_t v)   { *reinterpret_cast<volatile uint8_t*>(g_bar + o) = v; }
inline void wr16(uint32_t o, uint16_t v) { *reinterpret_cast<volatile uint16_t*>(g_bar + o) = v; }
inline void wr32(uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(g_bar + o) = v; }
inline void barrier() { asm volatile("mfence" ::: "memory"); }

bool g_ok = false, g_mute = false, g_playing = false;
int g_vol = 60, g_oss = 0, g_iss = 0, g_codec = 0, g_afg = 0, g_npins = 0;
uint32_t g_end_ms = 0, g_sd = 0;
volatile uint32_t* g_corb = nullptr; volatile uint32_t* g_rirb = nullptr;
uint32_t g_corb_n = 256, g_rirb_n = 256, g_wp = 0, g_rp = 0;
uint8_t* g_pcm = nullptr; uint64_t g_pcm_phys = 0; volatile uint32_t* g_bdl = nullptr; uint64_t g_bdl_phys = 0;
constexpr uint32_t kPcmBytes = 12 * 4096, kTag = 1;

void* dma(unsigned pages, uint64_t* phys) {
    const uint64_t p = pmm_alloc_contiguous(pages);
    if (!p || p >= 0x100000000ULL) return nullptr;
    memset(reinterpret_cast<void*>(p), 0, pages * 4096UL);
    *phys = p;
    return reinterpret_cast<void*>(p);
}
bool wait_bit(uint32_t off, uint32_t mask, uint32_t want, unsigned ms, int width) {
    const uint32_t t0 = tm::ms();
    for (;;) {
        const uint32_t v = width == 8 ? rd8(off) : (width == 16 ? rd16(off) : rd32(off));
        if ((v & mask) == want) return true;
        if (tm::ms() - t0 > ms) return false;
        tm::delay_us(50);
    }
}

// ---- codec commands ----
bool cmd(uint32_t verb, uint32_t* resp) {
    g_wp = (g_wp + 1) % g_corb_n;
    g_corb[g_wp] = verb;
    barrier();
    wr16(CORBWP, static_cast<uint16_t>(g_wp));
    const uint32_t t0 = tm::ms();
    for (;;) {
        if (static_cast<uint32_t>(rd16(RIRBWP) & 0xFF) != g_rp) {
            g_rp = (g_rp + 1) % g_rirb_n;
            barrier();
            const uint32_t r = g_rirb[g_rp * 2], ext = g_rirb[g_rp * 2 + 1];
            wr8(RIRBSTS, 0x5);
            if (ext & 0x10) continue;                 // unsolicited response: not ours
            if (resp) *resp = r;
            return true;
        }
        if (tm::ms() - t0 > 20) return false;
        tm::delay_us(20);
    }
}
inline uint32_t V12(int nid, uint32_t id, uint32_t payload) { return (static_cast<uint32_t>(g_codec) << 28) | (static_cast<uint32_t>(nid) << 20) | (id << 8) | (payload & 0xFF); }
inline uint32_t V4(int nid, uint32_t id, uint32_t payload) { return (static_cast<uint32_t>(g_codec) << 28) | (static_cast<uint32_t>(nid) << 20) | (id << 16) | (payload & 0xFFFF); }
uint32_t get(int nid, uint32_t id, uint32_t arg) { uint32_t r = 0; if (!cmd(V12(nid, id, arg), &r)) return 0; return r; }
uint32_t param(int nid, uint32_t p) { return get(nid, 0xF00, p); }
void set8(int nid, uint32_t id, uint32_t v) { cmd(V12(nid, id, v), nullptr); }

// ---- widget graph ----
struct Widget { uint8_t type = 0; uint32_t caps = 0; uint8_t nconn = 0; uint8_t conn[16] = {}; uint8_t sel = 0; bool on_path = false;
                uint32_t out_steps = 0, out_off = 0, in_off = 0; bool out_mut = false, has_out = false; };
Widget g_w[256];
uint8_t g_pins[4]; int g_dacs[4]; int g_ndacs = 0;
uint32_t afg_out_caps = 0, afg_in_caps = 0;

void read_conn(int nid) {
    Widget& w = g_w[nid];
    const uint32_t cl = param(nid, 0x0E);
    const bool lng = (cl >> 7) & 1; uint32_t n = cl & 0x7F; if (n > 16) n = 16;
    const uint32_t per = lng ? 2 : 4;
    for (uint32_t i = 0; i < n; i += per) {
        const uint32_t r = get(nid, 0xF02, i);
        for (uint32_t k = 0; k < per && i + k < n; ++k) w.conn[i + k] = lng ? ((r >> (16 * k)) & 0xFF) : ((r >> (8 * k)) & 0xFF);
    }
    w.nconn = static_cast<uint8_t>(n);
}
bool find_dac(int nid, int depth, bool mark) {
    Widget& w = g_w[nid];
    if (w.type == 0) { if (mark) w.on_path = true; return true; }
    if (depth > 5) return false;
    for (int i = 0; i < w.nconn; ++i) {
        const int child = w.conn[i];
        if (child == 0 || child == nid) continue;
        if (find_dac(child, depth + 1, mark)) { if (mark) { w.on_path = true; w.sel = static_cast<uint8_t>(i); } return true; }
    }
    return false;
}

void amp_set(int nid, bool out, int idx, bool mute, uint32_t gain) {
    uint32_t p = (out ? 0x8000u : 0x4000u) | 0x3000u | (static_cast<uint32_t>(idx) << 8) | (mute ? 0x80u : 0u) | (gain & 0x7F);
    cmd(V4(nid, 0x3, p), nullptr);
}
void apply_amps() {
    for (int nid = 0; nid < 256; ++nid) {
        Widget& w = g_w[nid];
        if (!w.on_path || !w.has_out) continue;
        const bool dac = w.type == 0;
        const uint32_t gain = dac ? (w.out_steps * static_cast<uint32_t>(g_vol) + 50) / 100 : w.out_steps;
        const bool mute = g_mute || (dac && g_vol == 0);
        if (w.out_mut || !mute) amp_set(nid, true, 0, mute, gain);
        else amp_set(nid, true, 0, false, 0);
    }
}

bool setup_graph() {
    uint32_t r = param(0, 0x04);
    int fstart = (r >> 16) & 0xFF, fcount = r & 0xFF;
    g_afg = 0;
    for (int i = 0; i < fcount; ++i) if ((param(fstart + i, 0x05) & 0xFF) == 1) { g_afg = fstart + i; break; }
    if (!g_afg) { log("no audio function group"); return false; }
    set8(g_afg, 0x705, 0);                                    // D0
    tm::delay_ms(10);
    afg_out_caps = param(g_afg, 0x12); afg_in_caps = param(g_afg, 0x0D);
    r = param(g_afg, 0x04);
    const int wstart = (r >> 16) & 0xFF, wcount = r & 0xFF;
    for (int i = 0; i < wcount && wstart + i < 256; ++i) {
        const int nid = wstart + i; Widget& w = g_w[nid];
        w.caps = param(nid, 0x09); w.type = static_cast<uint8_t>((w.caps >> 20) & 0xF);
        if (w.caps & 0x100) read_conn(nid);
        w.has_out = (w.caps >> 2) & 1;
        if (w.has_out) { const uint32_t c = (w.caps & 8) ? param(nid, 0x12) : afg_out_caps; w.out_steps = (c >> 8) & 0x7F; w.out_off = c & 0x7F; w.out_mut = (c >> 31) & 1; if (!c) w.has_out = false; }
        if ((w.caps >> 1) & 1) { const uint32_t c = (w.caps & 8) ? param(nid, 0x0D) : afg_in_caps; w.in_off = c & 0x7F; }
    }
    g_npins = 0; g_ndacs = 0;
    for (int i = 0; i < wcount && wstart + i < 256; ++i) {
        const int nid = wstart + i; Widget& w = g_w[nid];
        if (w.type != 4) continue;
        const uint32_t pcap = param(nid, 0x0C);
        if (!(pcap & 0x10)) continue;                          // not output capable
        const uint32_t cfg = get(nid, 0xF1C, 0);
        if (((cfg >> 30) & 3) == 1) continue;                  // no physical connection
        const uint32_t dev = (cfg >> 20) & 0xF;
        if (dev > 2) continue;                                 // line out, speaker, headphone only
        if (g_npins >= 4 || !find_dac(nid, 0, true)) continue;
        g_pins[g_npins++] = static_cast<uint8_t>(nid);
        set8(nid, 0x707, dev == 2 ? 0xC0 : 0x40);              // output enable (+ headphone amp)
        if (pcap & 0x10000) set8(nid, 0x70C, 0x02);            // EAPD on
    }
    if (!g_npins) { log("no output pins"); return false; }
    for (int nid = 0; nid < 256; ++nid) {
        Widget& w = g_w[nid];
        if (!w.on_path) continue;
        set8(nid, 0x705, 0);
        if (w.nconn > 1) set8(nid, 0x701, w.sel);
        if (((w.caps >> 1) & 1) && w.nconn) amp_set(nid, false, w.sel, false, w.in_off);   // unmute the input the path uses
        if (w.type == 0) {
            cmd(V4(nid, 0x2, 0x0011), nullptr);                // 48 kHz, 16 bit, 2 channels
            set8(nid, 0x706, kTag << 4);                       // stream tag 1, channel 0
            if (g_ndacs < 4) g_dacs[g_ndacs++] = nid;
        }
    }
    apply_amps();
    return true;
}

// ---- stream ----
void stream_stop() {
    if (!g_sd) return;
    wr32(g_sd + SD_CTL, rd32(g_sd + SD_CTL) & ~0x2u);
    g_playing = false;
}
bool stream_prepare() {
    stream_stop();
    wr32(g_sd + SD_CTL, 1);                                    // SRST
    if (!wait_bit(g_sd + SD_CTL, 1, 1, 10, 32)) return false;
    wr32(g_sd + SD_CTL, 0);
    if (!wait_bit(g_sd + SD_CTL, 1, 0, 10, 32)) return false;
    g_bdl[0] = static_cast<uint32_t>(g_pcm_phys); g_bdl[1] = static_cast<uint32_t>(g_pcm_phys >> 32); g_bdl[2] = kPcmBytes / 2; g_bdl[3] = 0;
    g_bdl[4] = static_cast<uint32_t>(g_pcm_phys + kPcmBytes / 2); g_bdl[5] = static_cast<uint32_t>((g_pcm_phys + kPcmBytes / 2) >> 32); g_bdl[6] = kPcmBytes / 2; g_bdl[7] = 0;
    wr32(g_sd + SD_BDLPL, static_cast<uint32_t>(g_bdl_phys)); wr32(g_sd + SD_BDLPU, static_cast<uint32_t>(g_bdl_phys >> 32));
    wr32(g_sd + SD_CBL, kPcmBytes); wr16(g_sd + SD_LVI, 1); wr16(g_sd + SD_FMT, 0x0011);
    wr8(g_sd + SD_STS, 0x1C);
    return true;
}

bool probe(const pcix::Dev& d) {
    uint64_t bar;
    if (!pcix::bar_mem(d, 0, &bar)) return false;
    pcix::enable(d);
    if (!vmm::map_mmio(bar, 0x4000, vmm::CacheMode::Uncached)) return false;
    g_bar = bar;
    const uint16_t gcap = rd16(GCAP);
    g_oss = (gcap >> 12) & 15; g_iss = (gcap >> 8) & 15;
    if (g_oss < 1) { log("controller has no output stream"); return false; }
    wr32(GCTL, rd32(GCTL) & ~1u);                              // controller reset
    if (!wait_bit(GCTL, 1, 0, 100, 32)) return false;
    tm::delay_ms(1);
    wr32(GCTL, rd32(GCTL) | 1u);
    if (!wait_bit(GCTL, 1, 1, 100, 32)) return false;
    uint16_t st = 0;
    for (int i = 0; i < 100 && !(st = rd16(STATESTS) & 0x7FFF); ++i) tm::delay_ms(1);
    if (!st) { log("no codec responded"); return false; }
    g_codec = 0; while (!((st >> g_codec) & 1)) ++g_codec;
    wr16(STATESTS, st);
    uint64_t p1, p2;
    uint8_t* ring = static_cast<uint8_t*>(dma(1, &p1));
    uint8_t* bdl = static_cast<uint8_t*>(dma(1, &p2));
    g_pcm = static_cast<uint8_t*>(dma(kPcmBytes / 4096, &g_pcm_phys));
    if (!ring || !bdl || !g_pcm) { log("out of DMA memory"); return false; }
    g_bdl = reinterpret_cast<volatile uint32_t*>(bdl); g_bdl_phys = p2;
    g_corb = reinterpret_cast<volatile uint32_t*>(ring); g_rirb = reinterpret_cast<volatile uint32_t*>(ring + 1024);
    // CORB
    wr8(CORBCTL, 0); wr8(RIRBCTL, 0);
    const uint8_t cs = rd8(CORBSIZE), rs = rd8(RIRBSIZE);
    if (cs & 0x40) { wr8(CORBSIZE, 2); g_corb_n = 256; } else if (cs & 0x20) { wr8(CORBSIZE, 1); g_corb_n = 16; } else { wr8(CORBSIZE, 0); g_corb_n = 2; }
    if (rs & 0x40) { wr8(RIRBSIZE, 2); g_rirb_n = 256; } else if (rs & 0x20) { wr8(RIRBSIZE, 1); g_rirb_n = 16; } else { wr8(RIRBSIZE, 0); g_rirb_n = 2; }
    wr32(CORBLBASE, static_cast<uint32_t>(p1)); wr32(CORBUBASE, static_cast<uint32_t>(p1 >> 32));
    wr32(RIRBLBASE, static_cast<uint32_t>(p1 + 1024)); wr32(RIRBUBASE, static_cast<uint32_t>((p1 + 1024) >> 32));
    wr16(CORBRP, 0x8000);
    if (!wait_bit(CORBRP, 0x8000, 0x8000, 10, 16)) return false;
    wr16(CORBRP, 0);
    if (!wait_bit(CORBRP, 0x8000, 0, 10, 16)) return false;
    wr16(CORBWP, 0); g_wp = 0;
    wr16(RIRBWP, 0x8000); g_rp = 0;
    wr16(RINTCNT, 1);
    wr8(CORBCTL, 0x02); wr8(RIRBCTL, 0x02);
    g_sd = 0x80 + 0x20 * static_cast<uint32_t>(g_iss);
    if (!setup_graph()) return false;
    if (!stream_prepare()) return false;
    return true;
}

}  // namespace

bool init() {
    g_ok = false;
    pcix::scan([](const pcix::Dev& d) { if (!g_ok && (d.cls >> 8) == 0x0403) g_ok = probe(d); });
    if (g_ok) log("audio ready");
    return g_ok;
}
bool available() { return g_ok; }
int  volume() { return g_vol; }
bool muted() { return g_mute; }
int  pin_count() { return g_ok ? g_npins : 0; }
void set_volume(int p) { if (p < 0) p = 0; if (p > 100) p = 100; g_vol = p; if (g_ok) apply_amps(); }
void set_mute(bool on) { g_mute = on; if (g_ok) apply_amps(); }

void tone(unsigned freq, unsigned ms) {
    if (!g_ok || g_mute || g_vol == 0 || freq < 50 || freq > 12000) return;
    if (ms > 250) ms = 250;
    const uint32_t frames = 48 * ms;
    if (!stream_prepare()) return;
    int16_t* s = reinterpret_cast<int16_t*>(g_pcm);
    const uint32_t period = 48000 / freq ? 48000 / freq : 1, amp = 9000;
    for (uint32_t i = 0; i < kPcmBytes / 4; ++i) {
        int v = 0;
        if (i < frames) {
            const uint32_t ph = i % period;                                    // triangle wave with fade in/out
            int tri = static_cast<int>(ph * 4 * amp / period); tri = ph < period / 2 ? tri - static_cast<int>(amp) : 3 * static_cast<int>(amp) - tri;
            uint32_t env = 256; if (i < 240) env = i * 256 / 240; if (frames - i < 240) env = (frames - i) * 256 / 240;
            v = static_cast<int>(static_cast<int>(tri) * static_cast<int>(env) / 256);
        }
        s[2 * i] = static_cast<int16_t>(v); s[2 * i + 1] = static_cast<int16_t>(v);
    }
    barrier();
    wr32(g_sd + SD_CTL, (kTag << 20) | 0x2);                    // tag 1 + RUN
    g_playing = true;
    g_end_ms = tm::ms() + ms + 30;
}

void poll() {
    if (g_playing && static_cast<int32_t>(tm::ms() - g_end_ms) > 0) stream_stop();
}

}  // namespace aos::audio
