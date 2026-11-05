// fd.hpp - per-process file descriptors, pipes, and console streams.
//
// Design goals:
//   * FDs 0/1/2 are stdin/stdout/stderr (console) for every new process
//   * open() on an AOSFS path returns a seekable FD (read or write)
//   * pipe() creates a pair of FDs backed by a shared ring buffer
//   * close() / dup() / inheritance on spawn work as expected
//   * All operations are process-local; the FS lock is taken only when touching the disk
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aos::process {

constexpr unsigned kMaxFds = 32;
constexpr unsigned kPipeCapacity = 4096;

enum class FdKind : uint8_t {
    Free = 0,
    Stdin,       // console input (line-buffered)
    Stdout,      // console output (serial)
    Stderr,      // same as stdout for now
    File,        // AOSFS path, offset-tracked
    PipeRead,
    PipeWrite,
};

enum : uint32_t {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR   = 2,
    O_CREAT  = 0x40,
    O_TRUNC  = 0x200,
    O_APPEND = 0x400,
};

struct Pipe {
    uint8_t  data[kPipeCapacity];
    uint32_t head;          // next read index
    uint32_t tail;          // next write index
    uint32_t len;           // bytes currently stored
    uint32_t readers;       // open read ends
    uint32_t writers;       // open write ends
    bool     in_use;
};

struct FileDesc {
    FdKind   kind;
    uint32_t flags;         // O_* bits
    uint32_t offset;        // for File
    char     path[64];      // for File (absolute or cwd-relative path as given)
    Pipe*    pipe;          // for PipeRead / PipeWrite
};

struct FdTable {
    FileDesc fds[kMaxFds];
};

// Process lifetime helpers
void fd_init_std(FdTable* t);                          // set up 0/1/2
void fd_close_all(FdTable* t);                         // on exit / reap
void fd_clone(const FdTable* src, FdTable* dst);       // spawn inheritance (deep-copies pipe refs)

// Syscall implementations (return negative errno on error, else >= 0)
int64_t fd_open(FdTable* t, const char* path, uint32_t flags);
int64_t fd_close(FdTable* t, int fd);
int64_t fd_read(FdTable* t, int fd, void* buf, size_t len);
int64_t fd_write(FdTable* t, int fd, const void* buf, size_t len);
int64_t fd_pipe(FdTable* t, int out_fds[2]);
int64_t fd_dup2(FdTable* t, int oldfd, int newfd);
int64_t fd_seek(FdTable* t, int fd, int64_t offset, int whence);  // SEEK_SET=0, CUR=1, END=2

// Kernel helpers for shell pipelines (not syscalls).
Pipe* pipe_create();
void  pipe_attach_read(FdTable* t, int fd, Pipe* p);
void  pipe_attach_write(FdTable* t, int fd, Pipe* p);
void  pipe_release_one(Pipe* p, bool was_reader);  // drop one reader or writer ref

}  // namespace aos::process
