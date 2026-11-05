#include "shell.hpp"
#include "../drivers/power.hpp"
#include "../drivers/audio/hda.hpp"
#include <stdarg.h>
#include "../arch/x86_64/interrupts.hpp"
#include "../memory/physical/pmm.h"
#include "../memory/heap/kheap.hpp"
#include "../scheduler/task.hpp"
#include "../filesystem/ramfs.hpp"
#include "../filesystem/page_cache.hpp"
#include "../cpu/cpu.h"
#include "../network/net.h"
#include "../drivers/rtc.hpp"
#include "../drivers/ps2_mouse.hpp"
#include "../drivers/storage-drivers/ata_pio.hpp"
#include "../drivers/storage-drivers/nvme.hpp"
#include "../drivers/storage-drivers/storage_mgr.hpp"

namespace aos::shell {

namespace {
const Sink* cur = nullptr;   // sink of the command being executed (the C net code has no context pointer)

size_t slen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
bool streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == *b;
}
bool starts_with(const char* s, const char* p) {
    while (*p) { if (*s != *p) return false; ++s; ++p; }
    return true;
}
const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}
// copies the first word of `src` into dst; returns pointer after it
const char* take_word(const char* src, char* dst, size_t n) {
    size_t i = 0;
    src = skip_spaces(src);
    while (*src && *src != ' ' && *src != '\t') { if (i + 1 < n) dst[i++] = *src; ++src; }
    dst[i] = 0;
    return src;
}

void put(const char* s, size_t n) { if (cur && cur->write) cur->write(cur->ctx, s, n); }
void puts(const char* s) { put(s, slen(s)); }
void putc(char c) { put(&c, 1); }
void putu(uint64_t v, unsigned base = 10) {
    char b[24]; int i = 0;
    if (!v) { putc('0'); return; }
    while (v) { unsigned d = (unsigned)(v % base); b[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; }
    while (i--) putc(b[i]);
}
void putd(int64_t v) { if (v < 0) { putc('-'); v = -v; } putu((uint64_t)v); }

// printf for the C network code: %s %d %u %x %c %%
extern "C" void shell_net_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { putc(*fmt); continue; }
        ++fmt;
        switch (*fmt) {
            case 's': { const char* s = va_arg(ap, const char*); puts(s ? s : "(null)"); break; }
            case 'd': putd((int)va_arg(ap, int)); break;
            case 'u': putu((unsigned)va_arg(ap, unsigned)); break;
            case 'x': case 'X': puts("0x"); putu((unsigned)va_arg(ap, unsigned), 16); break;
            case 'c': putc((char)va_arg(ap, int)); break;
            case '%': putc('%'); break;
            case 0: --fmt; break;
            default: putc('%'); putc(*fmt); break;
        }
    }
    va_end(ap);
}

void report(fs::Result r) { puts(fs::result_text(r)); putc('\n'); }

void cmd_mouse(const char* arg) {
    if (*arg >= '0' && *arg <= '9') {
        int v = 0;
        while (*arg >= '0' && *arg <= '9' && v < 1000) v = v * 10 + (*arg++ - '0');
        mouse_set_speed(v);
    }
    puts("mouse speed: "); putu(static_cast<unsigned>(mouse_get_speed())); puts("%  (usage: mouse <0-100>, default 22, 50 = about 1:1)\n");
}

void cmd_help() {
    puts(
        "AOS shell commands\n"
        "  help             this list\n"
        "  mouse [0-100]    show / set pointer speed\n"
        "  ver              kernel version\n"
        "  uptime           time since boot\n"
        "  date / time      CMOS real-time clock\n"
        "  mem              memory stats\n"
        "  cache            page-cache stats (hits/misses)\n"
        "  cpu              CPU info\n"
        "  disk / hdinfo    ATA primary master info\n"
        "  disks            NVMe disks + partitions (and which one AOS uses)\n"
        "  mkaosfs <d> <p> CONFIRM [FORCE]   format partition p (0 = blank whole disk) of disk d as AOSFS\n"
        "  tasks            list kernel threads (state, priority, CPU, run ticks)\n"
        "  ps               list user processes\n"
        "  exec <file> [args]  run an ELF program and wait (Ctrl-C kills it);  run <file> [args] = background\n"
        "  kill <pid>  nice <tid> <0-3>  usertest  schedtest [sec]\n"
        "  cpus  apic       processors, interrupt controller status\n"
        "  clear            clear the screen\n"
        "  echo <text>      print text\n"
        "  ls               list current directory\n"
        "  cd <path>        change directory (a/b, .., /)\n"
        "  pwd              print current directory\n"
        "  cat <file>       show a file\n"
        "  touch <file>     create empty file\n"
        "  mkdir <path>     create directory (parent must exist)\n"
        "  write <path> <txt>  write text to a file\n"
        "  rm <name>        delete file / empty dir\n"
        "  format CONFIRM   wipe the mounted AOSFS and start fresh\n"
        "  sync             flush filesystem to disk\n"
        "  ifconfig  lspci  arp  netpoll\n"
        "  ping <ip>  dhcp  nslookup <host>  httpget <host>\n"
        "  reboot           restart the machine\n"
        "  poweroff         shut the machine down\n"
        "  beep             play a test tone (HDA audio)\n");
}

void cmd_uptime() {
    const uint64_t s = timer_ticks() / 90;
    puts("up "); putu(s / 3600); puts("h "); putu((s / 60) % 60); puts("m "); putu(s % 60); puts("s\n");
}

void cmd_mem() {
    puts("Free memory : "); putu(pmm_free_frames() * 4 / 1024); puts(" MiB ("); putu(pmm_free_frames()); puts(" frames)\n");
    puts("Heap pages  : "); putu(heap::pages_in_use()); putc('\n');
}
void cmd_cache() {
    aos::page_cache::Stats s{};
    aos::page_cache::stats(&s);
    puts("Page cache\n");
    puts("  capacity : "); putu(s.capacity); puts(" slots ("); putu(s.capacity * 512 / 1024); puts(" KiB)\n");
    puts("  used     : "); putu(s.used); puts("\n");
    puts("  hits     : "); putu(s.hits); puts("\n");
    puts("  misses   : "); putu(s.misses); puts("\n");
    puts("  inserts  : "); putu(s.inserts); puts("\n");
    puts("  evictions: "); putu(s.evictions); puts("\n");
    puts("  invalid. : "); putu(s.invalidations); puts("\n");
    if (s.hits + s.misses)
        { puts("  hit rate : "); putu(s.hits * 100 / (s.hits + s.misses)); puts("%\n"); }
}

void cmd_cpu() {
    puts("Vendor : "); puts(g_cpu.vendor); putc('\n');
    puts("Brand  : "); puts(g_cpu.brand[0] ? g_cpu.brand : "(n/a)"); putc('\n');
    puts("Family "); putu(g_cpu.family); puts("  Model "); putu(g_cpu.model); puts("  Stepping "); putu(g_cpu.stepping); putc('\n');
    // Feature bits (common useful ones)
    puts("Features: ");
    if (g_cpu.features_edx & (1u << 0))  puts("FPU ");
    if (g_cpu.features_edx & (1u << 4))  puts("TSC ");
    if (g_cpu.features_edx & (1u << 5))  puts("MSR ");
    if (g_cpu.features_edx & (1u << 15)) puts("CMOV ");
    if (g_cpu.features_edx & (1u << 23)) puts("MMX ");
    if (g_cpu.features_edx & (1u << 25)) puts("SSE ");
    if (g_cpu.features_edx & (1u << 26)) puts("SSE2 ");
    if (g_cpu.features_ecx & (1u << 0))  puts("SSE3 ");
    if (g_cpu.features_ecx & (1u << 9))  puts("SSSE3 ");
    if (g_cpu.features_ecx & (1u << 19)) puts("SSE4.1 ");
    if (g_cpu.features_ecx & (1u << 20)) puts("SSE4.2 ");
    if (g_cpu.features_ecx & (1u << 28)) puts("AVX ");
    putc('\n');
}

void cmd_date() {
    aos::rtc::DateTime dt{};
    if (!aos::rtc::read(&dt)) {
        puts("RTC: unavailable or updating\n");
        return;
    }
    char buf[32];
    aos::rtc::format(dt, buf, sizeof(buf));
    puts(buf); putc('\n');
}

void put_size(uint64_t bytes) {
    const uint64_t mib = bytes / (1024 * 1024);
    if (mib >= 1024) { putu(mib / 1024); putc('.'); putu((mib % 1024) * 10 / 1024); puts(" GiB"); }
    else { putu(mib); puts(" MiB"); }
}

void cmd_disk() {
    for (int i = 0; i < aos::storage::nvme::namespace_count(); ++i) {
        const auto* n = aos::storage::nvme::info(i);
        puts("NVMe "); putu(i); puts(": "); puts(n->model); puts("  "); put_size(n->bytes); puts("  block "); putu(n->native_block); putc('\n');
    }
    const auto* info = aos::storage::ata_pio::primary_master_info();
    if (!info || !info->present) {
        puts("No ATA primary master detected\n");
        return;
    }
    puts("Model  : "); puts(info->model[0] ? info->model : "(unknown)"); putc('\n');
    puts("Serial : "); puts(info->serial[0] ? info->serial : "(unknown)"); putc('\n');
    puts("LBA48  : "); puts(info->lba48 ? "yes\n" : "no\n");
    puts("Sectors: "); putu(info->sectors); putc('\n');
    // size in MiB / GiB
    const uint64_t mib = (info->sectors * 512) / (1024 * 1024);
    if (mib >= 1024) {
        puts("Size   : "); putu(mib / 1024); puts("."); putu((mib % 1024) * 10 / 1024); puts(" GiB\n");
    } else {
        puts("Size   : "); putu(mib); puts(" MiB\n");
    }
}

void cmd_disks() {
    namespace mg = aos::storage::mgr;
    puts("Mounted: "); puts(fs::is_persistent() ? fs::device_label() : "nothing (no storage!)"); putc('\n');
    if (!mg::disk_count()) { puts("No NVMe disk found.\n"); return; }
    for (int d = 0; d < mg::disk_count(); ++d) {
        const auto* di = mg::disk(d);
        puts("disk "); putu(d); puts(": "); puts(di->model); puts("  "); put_size(di->bytes);
        puts(di->has_gpt ? "  GPT" : (di->blank ? "  blank" : "  no GPT")); putc('\n');
        for (int i = 0; i < mg::part_count(); ++i) {
            const auto* p = mg::part(i);
            if (p->disk != d) continue;
            puts("   part "); putu(p->index); puts(": "); put_size(p->sectors * 512); puts("  "); puts(p->kind);
            if (p->name[0]) { puts(" \""); puts(p->name); putc('"'); }
            if (p->aos_fs) puts("  [AOSFS in use]");
            else if (p->foreign_fs) puts("  [has a filesystem]");
            else puts("  [empty]");
            putc('\n');
        }
    }
    puts("To use a partition for AOS:  mkaosfs <disk> <part> CONFIRM\n");
}

int parse_int(const char* s) { int v = 0; if (!*s) return -1; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return *s ? -1 : v; }

void cmd_mkaosfs(const char* args) {
    namespace mg = aos::storage::mgr;
    char a[16], b[16], c[16], d[16];
    const char* r = take_word(args, a, sizeof a);
    r = take_word(r, b, sizeof b);
    r = take_word(r, c, sizeof c);
    take_word(r, d, sizeof d);
    const int disk = parse_int(a), part = parse_int(b);
    if (disk < 0 || part < 0 || !streq(c, "CONFIRM")) {
        puts("usage: mkaosfs <disk> <part> CONFIRM [FORCE]   (see: disks)\n"
             "part 0 = whole disk, only allowed on a blank disk. This ERASES that partition.\n");
        return;
    }
    const aos::storage::BlockDevice* dev = nullptr;
    puts("formatting...\n");
    switch (mg::format_target(disk, part, streq(d, "FORCE"), &dev)) {
        case mg::FormatResult::Ok: break;
        case mg::FormatResult::BadArgs: puts("no such disk/partition\n"); return;
        case mg::FormatResult::Refused: puts("refused: system/recovery partition, non-blank disk, tiny partition, or it still holds a filesystem (add FORCE if you are sure)\n"); return;
        case mg::FormatResult::IoError: puts("disk I/O error\n"); return;
    }
    const fs::Result m = fs::mount_device(dev);
    if (m == fs::Result::Ok) puts("done - AOSFS is mounted. Reboot to see it in Files with sample folders.\n"); else report(m);
}

void cmd_pwd() { char p[64]; fs::cwd_path(p, sizeof(p)); puts(p); putc('\n'); }

void ls_entry(const char* name, bool is_dir, size_t size, void*) {
    if (is_dir) { puts("  [dir]  "); puts(name); putc('/'); }
    else { puts("  "); putu(size); puts(" B"); for (int i = 0; i < 6; ++i) putc(' '); puts(name); }
    putc('\n');
}

void cmd_write(const char* args) {
    char name[96];
    const char* text = take_word(args, name, sizeof(name));
    text = skip_spaces(text);
    report(fs::write(name, text, slen(text)));
}

void cmd_cat(const char* args) {
    char name[96]; take_word(args, name, sizeof(name));
    const fs::Node* n = fs::read(name);
    if (!n) { puts("not found\n"); return; }
    for (size_t i = 0; i < n->size; ++i) putc(static_cast<char>(n->content[i]));
    if (!n->size || n->content[n->size - 1] != '\n') putc('\n');
}

bool net_ready() {
    if (g_netif.initialized) return true;
    puts("network not initialized (no supported NIC)\n");
    return false;
}
}  // namespace

void prompt(char* buf, size_t n) {
    char path[48];
    fs::cwd_path(path, sizeof(path));
    size_t i = 0;
    const char* parts[3] = {"aos:", path, "> "};
    for (const char* p : parts) while (*p && i + 1 < n) buf[i++] = *p++;
    buf[i] = 0;
}

void execute(const char* line, const Sink& out) {
    const Sink* saved = cur;
    cur = &out;
    line = skip_spaces(line);
    char arg[64];

    if (!*line) {}
    else if (system_commands(line, out)) {}
    else if (streq(line, "help") || streq(line, "?")) cmd_help();
    else if (streq(line, "ver")) puts(fs::is_persistent() ? "AOS Enhanced 64-bit + AOSFS (disk)\n" : "AOS Enhanced 64-bit (no storage mounted)\n");
    else if (streq(line, "uptime")) cmd_uptime();
    else if (streq(line, "date") || streq(line, "time")) cmd_date();
    else if (streq(line, "mem")) cmd_mem();
    else if (streq(line, "cache")) cmd_cache();
    else if (streq(line, "cpu")) cmd_cpu();
    else if (streq(line, "disk") || streq(line, "hdinfo")) cmd_disk();
    else if (streq(line, "disks") || streq(line, "lsblk")) cmd_disks();
    else if (starts_with(line, "mkaosfs")) cmd_mkaosfs(line + 7);
    else if (streq(line, "tasks")) { puts("tasks: "); putu(scheduler::task_count()); putc('\n'); }
    else if (streq(line, "clear")) { if (out.clear) out.clear(out.ctx); }
    else if (streq(line, "echo") || starts_with(line, "echo ")) { puts(skip_spaces(line + 4)); putc('\n'); }
    else if (streq(line, "ls") || starts_with(line, "ls ")) {
        const char* a = skip_spaces(line + 2);
        if (!*a) fs::list(ls_entry, nullptr);
        else {
            char path[96];
            take_word(a, path, sizeof path);
            fs::list_path(path, ls_entry, nullptr);
        }
    }
    else if (streq(line, "mouse") || starts_with(line, "mouse ")) cmd_mouse(skip_spaces(line + 5));
    else if (streq(line, "pwd")) cmd_pwd();
    else if (streq(line, "cd") || starts_with(line, "cd ")) {
        take_word(line + 2, arg, sizeof(arg));
        const fs::Result r = fs::cd(arg);
        if (r == fs::Result::Ok) cmd_pwd(); else report(r);
    }
    else if (starts_with(line, "cat ")) cmd_cat(line + 4);
    else if (starts_with(line, "touch ")) { take_word(line + 6, arg, 96); report(fs::create(arg)); }
    else if (starts_with(line, "mkdir ")) { take_word(line + 6, arg, 96); report(fs::mkdir(arg)); }
    else if (starts_with(line, "rm ")) { take_word(line + 3, arg, 96); report(fs::remove(arg)); }
    else if (starts_with(line, "mv ")) {
        char a[96], b[96];
        const char* r = take_word(line + 3, a, sizeof a);
        take_word(r, b, sizeof b);
        report(fs::rename(a, b));
    }
    else if (starts_with(line, "cp ")) {
        char a[96], b[96];
        const char* r = take_word(line + 3, a, sizeof a);
        take_word(r, b, sizeof b);
        report(fs::copy(a, b));
    }
    else if (starts_with(line, "write ")) cmd_write(line + 6);
    else if (streq(line, "ifconfig")) { if (net_ready()) net_cmd_ifconfig(shell_net_printf); }
    else if (streq(line, "lspci")) { if (net_ready()) net_cmd_lspci(shell_net_printf); }
    else if (streq(line, "arp")) { if (net_ready()) net_cmd_arp(shell_net_printf); }
    else if (starts_with(line, "ping ")) { take_word(line + 5, arg, sizeof(arg)); if (net_ready()) net_cmd_ping(arg, shell_net_printf); }
    else if (streq(line, "dhcp")) { if (net_ready()) net_cmd_dhcp(shell_net_printf); }
    else if (starts_with(line, "nslookup ")) { take_word(line + 9, arg, sizeof(arg)); if (net_ready()) net_cmd_nslookup(arg, shell_net_printf); }
    else if (starts_with(line, "httpget ")) { take_word(line + 8, arg, sizeof(arg)); if (net_ready()) net_cmd_httpget(arg, shell_net_printf); }
    else if (streq(line, "netpoll")) { net_poll(); puts("polled\n"); }
    else if (streq(line, "format")) puts("this erases all AOS files. Type: format CONFIRM\n");
    else if (streq(line, "format CONFIRM")) {
        puts("formatting disk...\n");
        report(fs::format());
        if (fs::is_persistent()) puts("(persistent AOSFS ready)\n");
    }
    else if (streq(line, "sync")) {
        puts(fs::sync() ? "synced\n" : "sync failed\n");
    }
    else if (streq(line, "beep")) {
        if (audio::available()) { audio::tone(880, 150); puts("beep\n"); } else puts("no audio device\n");
    }
    else if (streq(line, "reboot")) {
        puts("rebooting...\n");
        power::restart();
    }
    else if (streq(line, "poweroff") || streq(line, "shutdown")) {
        puts("shutting down...\n");
        power::shutdown();
    }
    else puts("unknown command (try help)\n");

    cur = saved;
}

}  // namespace aos::shell
