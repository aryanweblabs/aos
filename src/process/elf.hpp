// elf.hpp - minimal static ELF64 (x86-64, ET_EXEC) validation for the program loader.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::elf {

constexpr unsigned kMaxSegments = 8;
enum : uint32_t { PF_X = 1, PF_W = 2, PF_R = 4 };

struct Segment { uint64_t vaddr, offset, filesz, memsz; uint32_t flags; };
struct Image {
    uint64_t entry;
    unsigned segment_count;
    Segment  segments[kMaxSegments];
};

enum class Err { Ok, TooShort, BadMagic, NotElf64, NotX86_64, NotExecutable, BadProgramHeaders, BadSegment, TooManySegments, BadEntry, Overlap };

// Everything is validated here: the loader may trust the result.  user_base/user_end bound allowed addresses.
Err parse(const uint8_t* data, size_t length, uint64_t user_base, uint64_t user_end, Image* out);
const char* error_text(Err e);
}  // namespace aos::elf
