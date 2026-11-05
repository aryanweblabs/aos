// process.hpp - ring-3 processes: one address space + one kernel thread each.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../memory/virtual/aspace.hpp"
#include "../arch/x86_64/frame.hpp"
#include "fd.hpp"

namespace aos::process {

constexpr uint32_t kMaxProcesses = 32;
constexpr uint64_t kHeapBase  = 0x0000009000000000ULL;
constexpr uint64_t kMmapBase  = 0x000000A000000000ULL;
constexpr uint64_t kStackTop  = 0x000000B000000000ULL;
constexpr uint64_t kStackPages = 32;
constexpr uint64_t kMaxHeapBytes = 256ULL << 20;

enum class State : uint8_t { Free, Loading, Running, Zombie };

struct Process {
    uint32_t pid;
    char     name[24];
    State    state;
    volatile bool killed;
    int      exit_code;
    uint32_t task_id;
    uint32_t parent_task;
    uint64_t entry, user_rsp, argc, argv;
    uint64_t brk, mmap_next;
    aos::vmm::AddressSpace as;
    FdTable  fds;
    bool     forked_child;
    InterruptFrame fork_frame;
};

struct ProcInfo { uint32_t pid; char name[24]; uint8_t state; int exit_code; uint64_t pages; uint32_t task_id; };

struct SpawnStdio {
    Pipe* stdin_from  = nullptr;
    Pipe* stdout_to   = nullptr;
};

void init();
int  spawn(const uint8_t* image, size_t length, const char* name, const char* args,
           const char** error, const SpawnStdio* io = nullptr);
int  spawn_path(const char* path, const char* args, const char** error,
                const SpawnStdio* io = nullptr);
int  fork_from(InterruptFrame* frame);
bool wait_for(uint32_t pid, int* code, bool blocking);
bool kill(uint32_t pid);
Process* current();
[[noreturn]] void exit_current(int code);
[[noreturn]] void user_fault(InterruptFrame* frame);
void check_kill();
int  list(ProcInfo* out, int max);

void stdin_push(char c);
bool stdin_wanted();
long stdin_read(char* dst, size_t max);
void set_foreground(uint32_t pid);
uint32_t foreground();
}  // namespace aos::process
