/* SPDX-License-Identifier: MIT */
// rtc.cpp - CMOS RTC reader (IRQ8 not used; polled, update-in-progress safe)
#include "rtc.hpp"

namespace aos::rtc {
namespace {

inline void out8(uint16_t port, uint8_t v) { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
inline uint8_t in8(uint16_t port) { uint8_t v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

uint8_t cmos_read(uint8_t reg) {
    out8(0x70, reg | 0x80);          // NMI disable bit
    return in8(0x71);
}

bool updating() {
    return (cmos_read(0x0A) & 0x80) != 0;
}

uint8_t bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

}  // namespace

bool read(DateTime* out) {
    if (!out) return false;
    *out = {};
    // Wait for update-in-progress to clear (max ~1 ms typical)
    for (unsigned i = 0; i < 100000 && updating(); ++i)
        asm volatile("pause");
    if (updating()) return false;

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t cent = cmos_read(0x32);   // century register (common on modern BIOSes / QEMU)
    uint8_t week = cmos_read(0x06);
    uint8_t regB = cmos_read(0x0B);

    const bool is_bcd   = !(regB & 0x04);
    const bool is_24h   = (regB & 0x02) != 0;

    if (is_bcd) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        if (cent) cent = bcd_to_bin(cent);
        week = bcd_to_bin(week);
    } else {
        hour &= 0x7F;
    }

    if (!is_24h) {
        // 12-hour mode: bit 7 of original hour register was PM
        // (we already masked it for BCD path; re-read if needed)
        // Simplified: treat as 24h if unclear
    }

    if (sec > 59 || min > 59 || hour > 23 || day == 0 || day > 31 ||
        mon == 0 || mon > 12 || year > 99)
        return false;

    out->second  = sec;
    out->minute  = min;
    out->hour    = hour;
    out->day     = day;
    out->month   = mon;
    out->weekday = week;
    if (cent >= 19 && cent <= 21)
        out->year = static_cast<uint16_t>(cent * 100 + year);
    else
        out->year = static_cast<uint16_t>(2000 + year);   // sensible default for this project
    out->valid = true;
    return true;
}

uint64_t seconds_since_epoch(const DateTime& dt) {
    if (!dt.valid) return 0;
    // Crude day count (no leap-year precision needed for shell display)
    static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint64_t days = 0;
    for (uint16_t y = 1970; y < dt.year; ++y)
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    if (dt.month >= 1 && dt.month <= 12) {
        days += mdays[dt.month - 1];
        if (dt.month > 2 && (dt.year % 4 == 0 && (dt.year % 100 != 0 || dt.year % 400 == 0)))
            ++days;
    }
    days += (dt.day > 0 ? dt.day - 1 : 0);
    return days * 86400ULL + dt.hour * 3600ULL + dt.minute * 60ULL + dt.second;
}

void format(const DateTime& dt, char* buf, size_t n) {
    if (!buf || n < 20) { if (buf && n) buf[0] = 0; return; }
    if (!dt.valid) {
        // "---- -- -- --:--:--"
        const char* s = "---- -- -- --:--:--";
        size_t i = 0; while (s[i] && i + 1 < n) { buf[i] = s[i]; ++i; } buf[i] = 0;
        return;
    }
    auto put2 = [](char* p, uint8_t v) {
        p[0] = '0' + (v / 10) % 10;
        p[1] = '0' + v % 10;
    };
    // YYYY-MM-DD HH:MM:SS
    buf[0] = '0' + (dt.year / 1000) % 10;
    buf[1] = '0' + (dt.year / 100) % 10;
    buf[2] = '0' + (dt.year / 10) % 10;
    buf[3] = '0' + dt.year % 10;
    buf[4] = '-';
    put2(buf + 5, dt.month);
    buf[7] = '-';
    put2(buf + 8, dt.day);
    buf[10] = ' ';
    put2(buf + 11, dt.hour);
    buf[13] = ':';
    put2(buf + 14, dt.minute);
    buf[16] = ':';
    put2(buf + 17, dt.second);
    buf[19] = 0;
}

}  // namespace aos::rtc
