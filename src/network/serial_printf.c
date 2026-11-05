/* SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stdint.h>

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r;
}
static void sputc(char c) {
    static int dead = 0;
    if (dead) return;
    unsigned n = 0;
    while ((inb(0x3FD) & 0x20) == 0 && ++n < 20000) __asm__ volatile("pause");
    if (n >= 20000 || inb(0x3FD) == 0xFF) { dead = 1; return; }
    outb(0x3F8, (uint8_t)c);
}
static void sputs(const char *s) { while (*s) sputc(*s++); }
static void sputu(unsigned long long v, int base) {
    char b[32]; int i = 0;
    if (!v) { sputc('0'); return; }
    while (v) { int d = (int)(v % (unsigned)base); b[i++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v /= (unsigned)base; }
    while (i--) sputc(b[i]);
}

void aos_net_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { sputc(*fmt); continue; }
        ++fmt;
        if (*fmt == 's') { const char *s = va_arg(ap, const char*); sputs(s ? s : "(null)"); }
        else if (*fmt == 'd' || *fmt == 'u') { sputu((unsigned)va_arg(ap, unsigned), 10); }
        else if (*fmt == 'x' || *fmt == 'X') { sputs("0x"); sputu((unsigned)va_arg(ap, unsigned), 16); }
        else if (*fmt == 'c') { sputc((char)va_arg(ap, int)); }
        else if (*fmt == '%') { sputc('%'); }
        else { sputc('%'); sputc(*fmt); }
    }
    va_end(ap);
}
