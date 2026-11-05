/* SPDX-License-Identifier: MIT */
/* vmtest.c - comprehensive virtual-memory + process test for AOS.
 *
 * Covers:
 *   1. Basic ring-3 execution
 *   2. Demand paging (large anonymous mmap that is only partially touched)
 *   3. COW fork (parent and child write different values to a shared page)
 *   4. Stack growth / stack COW after fork
 *   5. Independent address spaces (child mutation invisible to parent)
 *   6. Process exit codes and wait
 *   7. Multiple sequential forks
 */
#include "libaos.h"

static int g_fail;

static void expect(int cond, const char* msg) {
    if (cond) {
        puts_("  OK  ");
        puts_(msg);
        puts_("\n");
    } else {
        puts_("  FAIL ");
        puts_(msg);
        puts_("\n");
        g_fail = 1;
    }
}

/* Touch `npages` consecutive pages starting at `p`, writing a unique pattern. */
static void touch_pages(volatile char* p, u64 npages, char tag) {
    for (u64 i = 0; i < npages; ++i) {
        p[i * 4096] = (char)(tag + (char)i);
    }
}

/* Verify the pattern written by touch_pages. */
static int check_pages(volatile char* p, u64 npages, char tag) {
    for (u64 i = 0; i < npages; ++i) {
        if (p[i * 4096] != (char)(tag + (char)i)) return 0;
    }
    return 1;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    g_fail = 0;
    puts_("\n======== AOS VM TEST SUITE ========\n");

    /* ---- 1. Basic execution ---- */
    puts_("[1] ring-3 execution\n");
    expect(getpid_() > 0, "getpid > 0");

    /* ---- 2. Demand paging via brk/mmap ---- */
    puts_("[2] demand paging (large sparse region)\n");
    {
        /* Request 256 pages (1 MiB) via mmap.  Only touch 4 of them. */
        const u64 npages = 256;
        i64 addr = sys(SYS_MMAP, npages * 4096, 0, 0);
        expect(addr > 0, "mmap 1 MiB returned a usable address");
        if (addr > 0) {
            volatile char* p = (volatile char*)(u64)addr;
            touch_pages(p, 4, 'A');
            expect(check_pages(p, 4, 'A'), "touched pages hold written data");
            /* Untouched pages must still fault-in as zero (read). */
            expect(p[100 * 4096] == 0, "untouched page reads as zero");
            expect(p[200 * 4096] == 0, "another untouched page reads as zero");
            /* Now write one of them and re-check. */
            p[100 * 4096] = 'Z';
            expect(p[100 * 4096] == 'Z', "demand-zero page becomes private after write");
        }
    }

    /* ---- 3. COW fork: shared page diverges on write ---- */
    puts_("[3] COW fork (shared page diverges)\n");
    {
        i64 addr = sys(SYS_MMAP, 4096, 0, 0);
        expect(addr > 0, "mmap one page for COW test");
        if (addr > 0) {
            volatile int* cell = (volatile int*)(u64)addr;
            *cell = 0xC0FFEE;

            i64 pid = fork_();
            expect(pid >= 0, "fork succeeded");
            if (pid == 0) {
                /* Child: must see the original value, then write a new one. */
                if (*cell != 0xC0FFEE) exit_(10);
                *cell = 0xDEAD;
                if (*cell != 0xDEAD) exit_(11);
                exit_(42);
            } else if (pid > 0) {
                int code = 0;
                i64 w = sys(SYS_WAIT, (u64)pid, (u64)&code, 0);
                expect(w == pid, "wait returned child pid");
                expect(code == 42, "child exit code 42");
                /* Parent must still see the original value. */
                expect(*cell == 0xC0FFEE, "parent page unchanged after child write (COW)");
            }
        }
    }

    /* ---- 4. Stack COW after fork ---- */
    puts_("[4] stack COW after fork\n");
    {
        volatile int stack_var = 99;
        i64 pid = fork_();
        expect(pid >= 0, "fork for stack test");
        if (pid == 0) {
            stack_var = 12345;
            /* Force a deeper stack frame. */
            char buf[64];
            for (int i = 0; i < 64; ++i) buf[i] = (char)(i + 1);
            if (stack_var != 12345) exit_(20);
            if (buf[63] != 64) exit_(21);
            exit_(43);
        } else if (pid > 0) {
            int code = 0;
            i64 w = sys(SYS_WAIT, (u64)pid, (u64)&code, 0);
            expect(w == pid, "wait (stack test) returned child pid");
            expect(code == 43, "child (stack) exit code 43");
            expect(stack_var == 99, "parent stack_var unchanged");
        }
    }

    /* ---- 5. Multiple sequential forks ---- */
    puts_("[5] sequential forks\n");
    {
        int ok_count = 0;
        for (int i = 0; i < 4; ++i) {
            i64 pid = fork_();
            if (pid < 0) break;
            if (pid == 0) {
                exit_(100 + i);
            }
            int code = 0;
            i64 w = sys(SYS_WAIT, (u64)pid, (u64)&code, 0);
            if (w == pid && code == 100 + i) ++ok_count;
        }
        expect(ok_count == 4, "four sequential fork/wait cycles");
    }

    /* ---- 6. Large COW region ---- */
    puts_("[6] large COW region (16 pages)\n");
    {
        const u64 npages = 16;
        i64 addr = sys(SYS_MMAP, npages * 4096, 0, 0);
        expect(addr > 0, "mmap 16 pages");
        if (addr > 0) {
            volatile char* p = (volatile char*)(u64)addr;
            touch_pages(p, npages, 'L');
            expect(check_pages(p, npages, 'L'), "16 pages written");

            i64 pid = fork_();
            expect(pid >= 0, "fork large region");
            if (pid == 0) {
                touch_pages(p, npages, 'C');
                if (!check_pages(p, npages, 'C')) exit_(30);
                exit_(44);
            } else if (pid > 0) {
                int code = 0;
                sys(SYS_WAIT, (u64)pid, (u64)&code, 0);
                expect(code == 44, "large-COW child exit 44");
                expect(check_pages(p, npages, 'L'), "parent large region unchanged");
            }
        }
    }

    /* ---- 7. munmap ---- */
    puts_("[7] munmap\n");
    {
        i64 addr = mmap_(8 * 4096);
        expect(addr > 0, "mmap 8 pages for munmap");
        if (addr > 0) {
            volatile char* p = (volatile char*)(u64)addr;
            touch_pages(p, 8, 'U');
            expect(check_pages(p, 8, 'U'), "pages written before munmap");
            /* Unmap the middle 4 pages. */
            i64 rc = munmap_((void*)(u64)(addr + 2 * 4096), 4 * 4096);
            expect(rc == 0, "munmap middle 4 pages");
            /* Edges must still be accessible. */
            expect(p[0] == 'U', "first page still mapped");
            expect(p[7 * 4096] == (char)('U' + 7), "last page still mapped");
            /* Full unmap of the remainder. */
            rc = munmap_((void*)(u64)addr, 2 * 4096);
            expect(rc == 0, "munmap first 2 pages");
            rc = munmap_((void*)(u64)(addr + 6 * 4096), 2 * 4096);
            expect(rc == 0, "munmap last 2 pages");
        }
    }

    /* ---- 8. mprotect ---- */
    puts_("[8] mprotect\n");
    {
        i64 addr = mmap_(4 * 4096);
        expect(addr > 0, "mmap 4 pages for mprotect");
        if (addr > 0) {
            volatile char* p = (volatile char*)(u64)addr;
            p[0] = 'W';
            expect(p[0] == 'W', "writable before mprotect");
            /* Make read-only. */
            i64 rc = mprotect_((void*)(u64)addr, 4 * 4096, PROT_READ);
            expect(rc == 0, "mprotect -> READ only");
            expect(p[0] == 'W', "still readable after mprotect");
            /* Restore write so we can clean up without faulting. */
            rc = mprotect_((void*)(u64)addr, 4 * 4096, PROT_READ | PROT_WRITE);
            expect(rc == 0, "mprotect -> READ|WRITE");
            p[0] = 'X';
            expect(p[0] == 'X', "writable again after mprotect");
            munmap_((void*)(u64)addr, 4 * 4096);
        }
    }

    /* ---- Summary ---- */
    puts_("\n======== SUMMARY ========\n");
    if (g_fail) {
        puts_("RESULT: SOME TESTS FAILED\n");
        return 1;
    }
    puts_("RESULT: ALL TESTS PASSED\n");
    return 0;
}
