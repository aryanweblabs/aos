// shell.hpp - the AOS command interpreter. It knows nothing about screens: every
// byte of output goes through a Sink (serial console).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::shell {

struct Sink {
    void (*write)(void* ctx, const char* text, size_t len);   // '\n' = newline
    void (*clear)(void* ctx);                                  // may be nullptr
    void* ctx;
};

void execute(const char* line, const Sink& out);   // run one command line
void prompt(char* buf, size_t n);                  // e.g. "aos:/home> "
// Scheduler / process / APIC commands (tasks, ps, exec, run, kill, nice, schedtest, usertest, cpus, apic).
bool system_commands(const char* line, const Sink& out);
// Called by commands that wait (exec): keeps input, watchdog and polling drivers alive meanwhile.
void set_input_pump(void (*pump)());                  // e.g. "aos:/home> "

}  // namespace aos::shell
