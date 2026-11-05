#!/usr/bin/env python3
"""Stand-in for qemu-system-x86_64 used ONLY to test tools/selftest.py itself (`selftest.py --fake`).
It imitates the AOS serial shell using the real output formats. It says nothing about the kernel.
FAKE_MODE: ok | hang | crash | pic-fallback | smp-short | leak | sched-unfair"""
import os, select, sys, time

mode = os.environ.get('FAKE_MODE', 'ok')
cpus = 1
if '-smp' in sys.argv:
    cpus = int(sys.argv[sys.argv.index('-smp') + 1])
out = sys.stdout.buffer
t0 = time.time()
files = {}
frames_free = 100000
procs = []                                   # (pid, name, state, code)
next_pid = 1


def w(s):
    out.write(s.replace('\n', '\r\n').encode()); out.flush()


def prompt():
    w('aos:/> ')


boot = ['[boot] Multiboot2 OK', '[boot] GDT/TSS + Heap + preemptive Scheduler OK', '[boot] Interrupts OK (8259 + PIT)']
if mode == 'pic-fallback':
    boot += ['[apic] LAPIC timer did not tick, staying on the 8259 PIC', '[boot] APIC not used: legacy PIC/PIT interrupts']
elif 'apic' in ' '.join(sys.argv) or True:
    boot += ['[boot] APIC: LAPIC timer + I/O APIC active, CPUs in MADT: %d' % cpus]
if mode == 'hang':
    w('\n'.join(boot[:2]) + '\n'); time.sleep(600)
if mode == 'crash':
    w('\n'.join(boot[:2]) + '\nAOS KERNEL PANIC: PAGE FAULT (vector 0xe, error 0x2)\nRIP=0x100200 CS=0x8 RSP=0x118000\nSystem halted.\n'); sys.exit(1)
w('\n'.join(boot) + '\n')
if cpus > 1:
    w('[smp] application processors started: %02d\n' % (cpus - (2 if mode == 'smp-short' else 1)))
w('[boot] Shell ready\n')
prompt()

line = b''
fg_until = 0
fg_name = ''
while True:
    if fg_until and time.time() >= fg_until:
        w('counter: done after a preempted busy loop\n[exit code 7]\n'); fg_until = 0; prompt()
    r, _, _ = select.select([sys.stdin.buffer], [], [], 0.05)
    if not r:
        continue
    ch = os.read(sys.stdin.fileno(), 1)
    if not ch:
        break
    if ch == b'\x03' and fg_until:
        w('^C\n[exit code -9]\n'); fg_until = 0; prompt(); continue
    if fg_until:
        continue
    if ch == b'\r':
        cmd = line.decode(); line = b''
        w('\n')
        a = cmd.split()
        c = a[0] if a else ''
        if c == 'uptime':
            s = int(time.time() - t0); w('up 0h %dm %ds\n' % (s // 60, s % 60))
        elif c == 'tasks':
            extra = '' if mode != 'leak' else '    9 spin-a           ready       2    -        5 kernel\n'
            w('  TID NAME             STATE    PRIO CPU  TICKS    KIND\n    1 kmain            running     1    0      120 kernel\n    2 idle0            ready       4    -        3 kernel\n' + extra +
              'threads: %d (idle threads not counted), preemptive, 90 Hz tick\n' % (1 if mode != 'leak' else 2))
        elif c == 'apic':
            w('APIC: active, interrupt controller = LAPIC + I/O APIC [xAPIC]\n' if mode != 'pic-fallback' else 'APIC: present but NOT active (fell back to 8259 PIC + PIT)\n')
        elif c == 'cpus':
            w('CPUs online: %d   MADT lists: %d\n' % (cpus - (1 if mode == 'smp-short' else 0), cpus))
        elif c == 'write':
            files[a[1]] = ' '.join(a[2:])
        elif c == 'cat':
            w(files.get(a[1], 'not found') + '\n')
        elif c == 'mem':
            w('Free memory : %d MiB (%d frames)\nHeap pages  : 4\n' % (frames_free * 4 // 1024, frames_free))
        elif c == 'schedtest':
            time.sleep(1.0)
            b = 60000000 if mode != 'sched-unfair' else 30000000
            w('schedtest: 3 busy threads that never yield (prio 2, 2, 3) for 3 s\n  spin-a   (prio 2): 61000000\n  spin-b   (prio 2): %d\n  spin-low (prio 3): 900000   (aging lets it run a little despite the others)\n' % b)
            if mode == 'leak':
                frames_free -= 500
        elif c == 'usertest':
            w('--- exec hello.elf\nHello from ring 3! pid=1, argc=1 [hello.elf]\nheap checksum = 1044480 (ok)\nmmap ok, kernel tick = 900\n[exit code 0]\n'
              '--- exec counter.elf\ncounter: 1 (tick 10)\ncounter: done after a preempted busy loop\n[exit code 7]\n'
              '--- exec fault.elf\nfault: writing to kernel memory...\n\n[proc 3 \'fault.elf\'] killed by exception 14 rip=0x8000000100 err=0x7 addr=0x1000\n[exit code 139]\nusertest done (fault.elf is expected to be killed by the kernel, not to crash it)\n')
        elif c == 'exec' and a[1] == 'hello.elf':
            w('Hello from ring 3! pid=4, argc=%d [%s]\nheap checksum = 1044480 (ok)\n[exit code 0]\n' % (len(a) - 1, ' '.join(a[1:])))
        elif c == 'exec' and a[1] == 'fsio.elf':
            w('fsio: ok\n[exit code 0]\n')
        elif c == 'run':
            procs.append((next_pid, a[1])); w('started pid %d\n' % next_pid); next_pid += 1
        elif c == 'ps':
            w('  PID NAME                     STATE    PAGES  EXIT\n')
            for p, nm in procs:
                w('%5d %-24s %-8s %6d %s\n' % (p, nm, 'exited' if p in killed else 'running', 5, '-9' if p in killed else '-') if (killed := globals().setdefault('killed', set())) is not None else '')
        elif c == 'kill':
            globals().setdefault('killed', set()).add(int(a[1])); w('signalled\n')
        elif c == 'exec' and a[1] == 'counter.elf':
            fg_until = time.time() + 1.5; fg_name = 'counter'; w('counter: 1 (tick 10)\n'); continue
        elif c:
            w('unknown command (try help)\n')
        prompt()
    elif ch in (b'\x7f', b'\b'):
        line = line[:-1]
    else:
        line += ch; out.write(ch); out.flush()
