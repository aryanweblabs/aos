#include "elf.hpp"

namespace aos::elf {
namespace {
inline uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }
constexpr uint64_t kMaxMem = 256ULL << 20;
}

Err parse(const uint8_t* d, size_t len, uint64_t ubase, uint64_t uend, Image* out) {
    if (!d || len < 64) return Err::TooShort;
    if (d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') return Err::BadMagic;
    if (d[4] != 2 || d[5] != 1) return Err::NotElf64;                  // 64-bit, little endian
    if (u16(d + 18) != 62) return Err::NotX86_64;
    if (u16(d + 16) != 2) return Err::NotExecutable;                   // ET_EXEC only (no relocations)
    const uint64_t phoff = u64(d + 32);
    const unsigned phentsize = u16(d + 54), phnum = u16(d + 56);
    if (phentsize != 56 || phnum == 0 || phnum > 32 || phoff > len || phoff + static_cast<uint64_t>(phnum) * 56 > len)
        return Err::BadProgramHeaders;
    *out = {};
    out->entry = u64(d + 24);
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t* ph = d + phoff + static_cast<uint64_t>(i) * 56;
        if (u32(ph) != 1) continue;                                    // PT_LOAD only
        if (out->segment_count == kMaxSegments) return Err::TooManySegments;
        Segment s{u64(ph + 16), u64(ph + 8), u64(ph + 32), u64(ph + 40), u32(ph + 4)};
        if (s.memsz == 0) continue;
        if (s.filesz > s.memsz || s.memsz > kMaxMem) return Err::BadSegment;
        if (s.offset > len || s.filesz > len - s.offset) return Err::BadSegment;
        if (s.vaddr < ubase || s.vaddr >= uend || s.memsz > uend - s.vaddr) return Err::BadSegment;
        if ((s.vaddr & 4095) != (s.offset & 4095)) return Err::BadSegment;
        for (unsigned k = 0; k < out->segment_count; ++k) {            // no two segments may share a page
            const Segment& o = out->segments[k];
            const uint64_t a0 = s.vaddr & ~4095ULL, a1 = (s.vaddr + s.memsz + 4095) & ~4095ULL;
            const uint64_t b0 = o.vaddr & ~4095ULL, b1 = (o.vaddr + o.memsz + 4095) & ~4095ULL;
            if (a0 < b1 && b0 < a1) return Err::Overlap;
        }
        out->segments[out->segment_count++] = s;
    }
    if (!out->segment_count) return Err::BadSegment;
    bool entry_ok = false;
    for (unsigned k = 0; k < out->segment_count; ++k) {
        const Segment& s = out->segments[k];
        if ((s.flags & PF_X) && out->entry >= s.vaddr && out->entry < s.vaddr + s.memsz) entry_ok = true;
    }
    return entry_ok ? Err::Ok : Err::BadEntry;
}

const char* error_text(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::TooShort: return "file too short";
        case Err::BadMagic: return "not an ELF file";
        case Err::NotElf64: return "not a 64-bit little-endian ELF";
        case Err::NotX86_64: return "not an x86-64 binary";
        case Err::NotExecutable: return "not a static executable (ET_EXEC)";
        case Err::BadProgramHeaders: return "bad program headers";
        case Err::BadSegment: return "segment outside user space (link at 0x8000000000, see user/user.ld)";
        case Err::TooManySegments: return "too many segments";
        case Err::BadEntry: return "entry point not in an executable segment";
        case Err::Overlap: return "segments share a page (use -z max-page-size=4096)";
    }
    return "?";
}
}  // namespace aos::elf
