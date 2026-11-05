/* SPDX-License-Identifier: MIT */
// rtc.hpp - CMOS Real-Time Clock (MC146818)
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace aos::rtc {

struct DateTime {
    uint8_t  second, minute, hour;
    uint8_t  day, month;
    uint16_t year;          // full year e.g. 2026
    uint8_t  weekday;       // 0 = Sunday … 6 = Saturday (CMOS value, may be 0)
    bool     valid;
};

// Read current wall-clock time from CMOS. Returns false if CMOS is updating
// or values look invalid (then *out is zeroed).
bool read(DateTime* out);

// Seconds since an arbitrary epoch (rough; uses RTC fields only, no timezone).
// Useful for simple "unix-like" timestamps in shell.
uint64_t seconds_since_epoch(const DateTime& dt);

// Format into a short ASCII buffer (caller provides >= 32 bytes).
// e.g. "2026-10-15 21:34:07"
void format(const DateTime& dt, char* buf, size_t n);

}  // namespace aos::rtc
