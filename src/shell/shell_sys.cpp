// shell_sys.cpp - commands for the scheduler, user processes and the interrupt controller.
#include "shell.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../arch/x86_64/interrupts.hpp"
#include "../arch/x86_64/percpu.hpp"
#include "../memory/physical/pmm.h"
#include "../process/process.hpp"
#include "../scheduler/task.hpp"

namespace aos::shell {
namespace {
void (*g_pump)() = nullptr;
const Sink* g_out = nullptr;

size_t slen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
bool starts(const char* s, const char* p) { while (*p) { if (*s != *p) return false; ++s; ++p; } return true; }
bool is_cmd(const char* line, const char* cmd) {
    const size_t n = slen(cmd);
    return starts(line, cmd) && (line[n] == 0 || line[n] == ' ');
}
const char* skip(const char* s) { while (*s == ' ' || *s == '\t') ++s; return s; }

void puts(const char* s) { g_out->write(g_out->ctx, s, slen(s)); }
void putc(char c) { g_out->write(g_out->ctx, &c, 1); }
void putu(uint64_t v) {
    char b[21]; int n = 20; b[n] = 0;
    if (!v) b[--n] = '0';
    while (v) { b[--n] = static_cast<char>('0' + v % 10); v /= 10; }
    puts(&b[n]);
}
void putd(int64_t v) { if (v < 0) { putc('-'); v = -v; } putu(static_cast<uint64_t>(v)); }
void pad(const char* s, unsigned w) { puts(s); for (size_t n = slen(s); n < w; ++n) putc(' '); }
void padu(uint64_t v, unsigned w) {
    char b[21]; int n = 20; b[n] = 0; uint64_t t = v;
    if (!t) b[--n] = '0';
    while (t) { b[--n] = static_cast<char>('0' + t % 10); t /= 10; }
    for (size_t k = slen(&b[n]); k < w; ++k) putc(' ');
    puts(&b[n]);
}
bool parse_u(const char*& s, uint64_t* out) {
    s = skip(s);
    if (*s < '0' || *s > '9') return false;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + static_cast<unsigned>(*s++ - '0');
    *out = v;
    return true;
}
void apic_print(const char* t) { puts(t); }

void cmd_tasks() {
    aos::scheduler::TaskInfo info[aos::scheduler::kMaxTasks];
    const int n = aos::scheduler::snapshot(info, aos::scheduler::kMaxTasks);
    puts("  TID NAME             STATE    PRIO CPU  TICKS    KIND\n");
    for (int i = 0; i < n; ++i) {
        padu(info[i].id, 5); putc(' ');
        pad(info[i].name, 16); putc(' ');
        pad(aos::scheduler::state_name(info[i].state), 8); putc(' ');
        padu(info[i].prio, 4); putc(' ');
        if (info[i].cpu < 0) puts("   -"); else padu(static_cast<uint64_t>(info[i].cpu), 4);
        putc(' ');
        padu(info[i].run_ticks, 8); putc(' ');
        puts(info[i].user ? "user process" : "kernel"); putc('\n');
    }
    puts("threads: "); putu(aos::scheduler::task_count()); puts(" (idle threads not counted), preemptive, 90 Hz tick\n");
}

void cmd_ps() {
    aos::process::ProcInfo info[aos::process::kMaxProcesses];
    const int n = aos::process::list(info, aos::process::kMaxProcesses);
    if (!n) { puts("no user processes\n"); return; }
    puts("  PID NAME                     STATE    PAGES  EXIT\n");
    for (int i = 0; i < n; ++i) {
        padu(info[i].pid, 5); putc(' ');
        pad(info[i].name, 24); putc(' ');
        pad(info[i].state == static_cast<uint8_t>(aos::process::State::Zombie) ? "exited" : "running", 8); putc(' ');
        padu(info[i].pages, 6); putc(' ');
        if (info[i].state == static_cast<uint8_t>(aos::process::State::Zombie)) putd(info[i].exit_code); else putc('-');
        putc('\n');
    }
}

// Run a program; wait=true blocks (while pumping input) and prints the exit code.
int run_program(const char* line, bool wait, const aos::process::SpawnStdio* io = nullptr) {
    char path[64]; size_t i = 0;
    line = skip(line);
    while (*line && *line != ' ' && *line != '|' && i + 1 < sizeof(path)) path[i++] = *line++;
    path[i] = 0;
    if (!i) { puts("usage: exec <file> [args]\n"); return -1; }
    const char* err = nullptr;
    const int pid = aos::process::spawn_path(path, skip(line), &err, io);
    if (pid < 0) { puts("cannot run "); puts(path); puts(": "); puts(err ? err : "error"); putc('\n'); return -1; }
    if (!wait) { puts("started pid "); putu(static_cast<uint64_t>(pid)); putc('\n'); return pid; }
    aos::process::set_foreground(static_cast<uint32_t>(pid));
    int code = 0;
    while (!aos::process::wait_for(static_cast<uint32_t>(pid), &code, false)) {
        if (g_pump) g_pump();
        aos::scheduler::yield();
        aos::scheduler::idle_wait(timer_ticks());
    }
    aos::process::set_foreground(0);
    puts("[exit code "); putd(code); puts("]\n");
    return pid;
}

// Pipeline:  left | right   where both sides are executable names + args.
// Example:  exec pipedemo.elf   still works;  for two progs:  hello.elf | counter.elf is not ideal
// because those programs do not use stdin.  Real use is any writer | any reader.
bool run_pipeline(const char* line) {
    // Find the first '|' that is not inside a word oddly — simple scan.
    const char* bar = nullptr;
    for (const char* s = line; *s; ++s) if (*s == '|') { bar = s; break; }
    if (!bar) return false;

    char left[96], right[96];
    size_t li = 0, ri = 0;
    for (const char* s = line; s < bar && li + 1 < sizeof(left); ++s) left[li++] = *s;
    left[li] = 0;
    for (const char* s = bar + 1; *s && ri + 1 < sizeof(right); ++s) right[ri++] = *s;
    right[ri] = 0;
    // trim
    const char* L = skip(left);
    const char* R = skip(right);
    if (!*L || !*R) { puts("usage: <prog> [args] | <prog> [args]\n"); return true; }

    aos::process::Pipe* pipe = aos::process::pipe_create();
    if (!pipe) { puts("pipe: out of resources\n"); return true; }

    aos::process::SpawnStdio left_io{}, right_io{};
    left_io.stdout_to = pipe;
    right_io.stdin_from = pipe;

    const char* err = nullptr;
    // Start writer first (stdout -> pipe), do not wait yet.
    char lpath[64]; size_t i = 0;
    const char* lp = L;
    while (*lp && *lp != ' ' && i + 1 < sizeof(lpath)) lpath[i++] = *lp++;
    lpath[i] = 0;
    int pid_w = aos::process::spawn_path(lpath, skip(lp), &err, &left_io);
    if (pid_w < 0) {
        puts("cannot run left: "); puts(err ? err : "error"); putc('\n');
        aos::process::pipe_release_one(pipe, false);
        aos::process::pipe_release_one(pipe, true);
        return true;
    }

    char rpath[64]; i = 0;
    const char* rp = R;
    while (*rp && *rp != ' ' && i + 1 < sizeof(rpath)) rpath[i++] = *rp++;
    rpath[i] = 0;
    err = nullptr;
    int pid_r = aos::process::spawn_path(rpath, skip(rp), &err, &right_io);
    if (pid_r < 0) {
        puts("cannot run right: "); puts(err ? err : "error"); putc('\n');
        aos::process::kill(static_cast<uint32_t>(pid_w));
        aos::process::pipe_release_one(pipe, false);
        aos::process::pipe_release_one(pipe, true);
        return true;
    }

    // Shell holds no pipe ends; children own the refs from attach.
    // Wait for both.
    int code = 0;
    aos::process::set_foreground(static_cast<uint32_t>(pid_r));
    while (!aos::process::wait_for(static_cast<uint32_t>(pid_w), &code, false)) {
        if (g_pump) g_pump();
        aos::scheduler::yield();
        aos::scheduler::idle_wait(timer_ticks());
    }
    puts("[left exit "); putd(code); puts("] ");
    while (!aos::process::wait_for(static_cast<uint32_t>(pid_r), &code, false)) {
        if (g_pump) g_pump();
        aos::scheduler::yield();
        aos::scheduler::idle_wait(timer_ticks());
    }
    aos::process::set_foreground(0);
    puts("[right exit "); putd(code); puts("]\n");
    return true;
}

// ---- preemption demonstration: busy loops that never yield
volatile uint64_t g_count[3];
void spinner(void* arg) {
    const unsigned k = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
    while (!aos::scheduler::should_stop()) ++g_count[k];
}

void cmd_schedtest(uint64_t seconds) {
    if (!seconds || seconds > 20) seconds = 3;
    for (auto& c : g_count) c = 0;
    const uint8_t prios[3] = {aos::scheduler::PRIO_NORMAL, aos::scheduler::PRIO_NORMAL, aos::scheduler::PRIO_LOW};
    int ids[3];
    puts("schedtest: 3 busy threads that never yield (prio 2, 2, 3) for "); putu(seconds); puts(" s\n");
    for (unsigned i = 0; i < 3; ++i) {
        aos::scheduler::SpawnParams sp;
        sp.name = i == 0 ? "spin-a" : (i == 1 ? "spin-b" : "spin-low");
        sp.entry = spinner; sp.arg = reinterpret_cast<void*>(static_cast<uintptr_t>(i)); sp.prio = prios[i];
        ids[i] = aos::scheduler::spawn(sp);
        if (ids[i] < 0) puts("  could not start a thread\n");
    }
    const uint64_t end = timer_ticks() + seconds * 90;
    while (timer_ticks() < end) {
        if (g_pump) g_pump();                       // the shell thread itself keeps running: proof it preempts them
        aos::scheduler::yield();
        aos::scheduler::idle_wait(timer_ticks());
    }
    for (int id : ids) if (id > 0) aos::scheduler::kill(static_cast<uint32_t>(id));
    aos::scheduler::sleep_current(20);
    puts("  spin-a   (prio 2): "); putu(g_count[0]); putc('\n');
    puts("  spin-b   (prio 2): "); putu(g_count[1]); putc('\n');
    puts("  spin-low (prio 3): "); putu(g_count[2]); puts("   (aging lets it run a little despite the others)\n");
    puts("equal priorities should be within a few percent of each other; the shell stayed responsive throughout.\n");
}

void cmd_usertest() {
    static const char* const progs[] = {"hello.elf", "counter.elf", "fault.elf"};
    for (const char* p : progs) {
        puts("--- exec "); puts(p); putc('\n');
        run_program(p, true);
    }
    puts("usertest done (fault.elf is expected to be killed by the kernel, not to crash it)\n");
}

void cmd_cpus() {
    const unsigned n = aos::arch::cpu_count();
    puts("CPUs online: "); putu(n); puts("   MADT lists: "); putu(aos::apic::cpu_count()); putc('\n');
    for (unsigned i = 0; i < n; ++i) {
        const aos::arch::PerCpu* c = aos::arch::cpu_by_id(i);
        if (!c) continue;
        puts("  cpu "); putu(c->cpu_id); puts(i == 0 ? " (BSP)" : " (AP) "); puts(" lapic id "); putu(c->lapic_id);
        puts(" preempt_count "); putu(c->preempt_count); putc('\n');
    }
}
}  // namespace

void set_input_pump(void (*pump)()) { g_pump = pump; }

bool system_commands(const char* line, const Sink& out) {
    g_out = &out;
    uint64_t a = 0, b = 0;
    // Pipeline: any line containing '|' with two program sides.
    for (const char* s = line; *s; ++s) {
        if (*s == '|') return run_pipeline(line);
    }
    if (is_cmd(line, "tasks")) { cmd_tasks(); return true; }
    if (is_cmd(line, "ps")) { cmd_ps(); return true; }
    if (is_cmd(line, "exec")) { run_program(line + 4, true); return true; }
    if (is_cmd(line, "run")) { run_program(line + 3, false); return true; }
    if (is_cmd(line, "kill")) {
        const char* p = line + 4;
        if (!parse_u(p, &a)) { puts("usage: kill <pid>\n"); return true; }
        puts(aos::process::kill(static_cast<uint32_t>(a)) ? "signalled\n" : "no such running process\n");
        return true;
    }
    if (is_cmd(line, "nice")) {
        const char* p = line + 4;
        if (!parse_u(p, &a) || !parse_u(p, &b)) { puts("usage: nice <tid> <0-3>   (0 = most urgent)\n"); return true; }
        puts(aos::scheduler::set_priority(static_cast<uint32_t>(a), static_cast<uint8_t>(b)) ? "ok\n" : "failed (bad tid or priority)\n");
        return true;
    }
    if (is_cmd(line, "schedtest")) {
        const char* p = line + 9;
        if (!parse_u(p, &a)) a = 3;
        cmd_schedtest(a);
        return true;
    }
    if (is_cmd(line, "usertest")) { cmd_usertest(); return true; }
    if (is_cmd(line, "cpus")) { cmd_cpus(); return true; }
    if (is_cmd(line, "apic")) { aos::apic::describe(apic_print); return true; }
    return false;
}
}  // namespace aos::shell
