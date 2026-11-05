#!/usr/bin/env python3
"""AOS one-command self test: install tools -> build -> boot in QEMU -> run every test -> write a report.

    python3 tools/selftest.py            # full run (default, legacy PIC, higher half, 4-CPU SMP)
    python3 tools/selftest.py --quick    # default configuration only
    python3 tools/selftest.py --no-install --keep-going

Output: selftest_report/REPORT.md, per-configuration serial logs, selftest_report.zip (send that one back).
"""
import argparse, datetime, os, platform, re, select, shutil, subprocess, sys, time, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORT = os.path.join(ROOT, 'selftest_report')
PROMPT = re.compile(rb'aos:[^\r\n]*> $')
APT = ['build-essential', 'grub-pc-bin', 'grub-common', 'xorriso', 'mtools', 'qemu-system-x86', 'qemu-utils', 'python3']

results = []          # (config, check, status, detail)


def rec(cfg, name, status, detail=''):
    results.append((cfg, name, status, detail))
    mark = {'PASS': 'PASS', 'FAIL': 'FAIL', 'WARN': 'WARN', 'SKIP': 'skip'}[status]
    print('  [%s] %-34s %s' % (mark, name, detail.split('\n')[0][:90]), flush=True)


def sh(cmd, log=None, timeout=900, env=None):
    p = subprocess.run(cmd, cwd=ROOT, shell=isinstance(cmd, str), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, env=env)
    out = p.stdout.decode(errors='replace')
    if log:
        with open(os.path.join(REPORT, log), 'w') as f:
            f.write(out)
    return p.returncode, out


# ------------------------------------------------------------------ tools
def ensure_tools(args):
    need = {'qemu-system-x86_64': 'qemu-system-x86', 'grub-mkrescue': 'grub-common', 'xorriso': 'xorriso',
            'g++': 'build-essential', 'ld': 'build-essential', 'make': 'build-essential', 'qemu-img': 'qemu-utils'}
    if args.fake:
        return True
    missing = sorted({pkg for tool, pkg in need.items() if not shutil.which(tool)})
    if not missing:
        return True
    print('missing tools (packages):', ', '.join(missing))
    if args.no_install or not shutil.which('apt-get'):
        print('install them with:  sudo apt-get install -y ' + ' '.join(APT))
        return False
    pre = [] if os.geteuid() == 0 else ['sudo']
    print('installing with apt-get (sudo may ask for your password once)...')
    rc = subprocess.call(pre + ['apt-get', 'update', '-qq'])
    rc |= subprocess.call(pre + ['apt-get', 'install', '-y'] + APT)
    return rc == 0 and all(shutil.which(t) for t in need)


# ------------------------------------------------------------------ serial console on QEMU's stdio
class Console:
    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.buf = b''
        self.all = b''
        os.set_blocking(self.p.stdout.fileno(), False)

    def alive(self):
        return self.p.poll() is None

    def _pump(self, wait):
        r, _, _ = select.select([self.p.stdout], [], [], wait)
        if r:
            try:
                d = os.read(self.p.stdout.fileno(), 65536)
            except BlockingIOError:
                return
            if d:
                self.buf += d
                self.all += d

    def read_until_prompt(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self._pump(0.1)
            if PROMPT.search(self.buf):
                out, self.buf = self.buf, b''
                return out.decode(errors='replace'), True
            if not self.alive():
                self._pump(0.2)
                out, self.buf = self.buf, b''
                return out.decode(errors='replace'), False
        out, self.buf = self.buf, b''
        return out.decode(errors='replace'), False

    def send(self, text):
        try:
            self.p.stdin.write(text if isinstance(text, bytes) else text.encode())
            self.p.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def run(self, cmd, timeout=20):
        self.send(cmd + '\r')
        return self.read_until_prompt(timeout)

    def close(self):
        if self.alive():
            self.p.kill()
        try:
            self.p.wait(timeout=5)
        except Exception:
            pass


# ------------------------------------------------------------------ configurations
CONFIGS = [
    # name, make options, qemu cpus, what to check
    dict(name='default', hh=0, smp=0, apic=1, cpus=1, full=True),
    dict(name='legacy-pic', hh=0, smp=0, apic=0, cpus=1, full=False),
    dict(name='higher-half', hh=1, smp=0, apic=1, cpus=1, full=True),
    dict(name='smp4', hh=0, smp=1, apic=1, cpus=4, full=False),
]


def build(cfg, args):
    d = 'build/hh%d_smp%d_apic%d' % (cfg['hh'], cfg['smp'], cfg['apic'])
    cfg['dir'] = d
    if args.fake:
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
        cfg['iso'] = os.path.join(ROOT, d, 'aos.iso')
        open(cfg['iso'], 'wb').close()
        return True
    opts = ['HH=%d' % cfg['hh'], 'SMP=%d' % cfg['smp'], 'APIC=%d' % cfg['apic']]
    rc, out = sh(['make', '-j%d' % (os.cpu_count() or 2)] + opts + [d + '/aos.iso'], log='build_%s.log' % cfg['name'])
    if rc != 0:
        tail = '\n'.join(out.splitlines()[-8:])
        rec(cfg['name'], 'build', 'FAIL', 'make failed, see build_%s.log\n' % cfg['name'] + tail)
        print('    --- last lines of the build log ---')
        for l in tail.splitlines():
            print('    ' + l)
        return False
    warn = [l for l in out.splitlines() if 'warning' in l and 'aosfs.cpp' not in l and 'RWX' not in l and 'build-id' not in l]
    rec(cfg['name'], 'build', 'PASS' if not warn else 'WARN', '%d new warnings' % len(warn) if warn else 'clean')
    rc, out = sh(['python3', 'tools/check_kernel.py', d + '/kernel.elf', str(cfg['hh'])])
    rec(cfg['name'], 'static ELF check', 'PASS' if rc == 0 else 'FAIL', out.strip().splitlines()[-1] if out.strip() else '')
    cfg['iso'] = os.path.join(ROOT, d, 'aos.iso')
    return True


def qemu_argv(cfg, disk):
    q = os.environ.get('AOS_SELFTEST_QEMU', 'qemu-system-x86_64')
    argv = q.split() + ['-cdrom', cfg['iso'], '-drive', 'file=%s,format=raw,if=ide,index=0' % disk, '-m', '512M',
                        '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-smp', str(cfg['cpus']),
                        '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0']
    if os.access('/dev/kvm', os.W_OK) and 'AOS_SELFTEST_QEMU' not in os.environ:
        argv += ['-enable-kvm', '-cpu', 'host']
    return argv


def frames(text):
    m = re.search(r'\((\d+) frames\)', text)
    return int(m.group(1)) if m else None


def summary_line(out):
    """First meaningful line of a command's output (skips the echoed command and the prompt)."""
    for l in out.replace('\r', '').splitlines():
        l = l.strip()
        if l and not l.startswith('aos:') and not re.match(r'^[A-Za-z0-9_ ./-]{1,40}$', l) is None and False:
            return l
        if l and not l.startswith('aos:'):
            return l
    return ''


def has(out, pat):
    return re.search(pat, out) is not None


# ------------------------------------------------------------------ the tests (run against a booted console)
def test_boot(cfg, boot):
    n = cfg['name']
    rec(n, 'boot: GDT/TSS+scheduler', 'PASS' if 'GDT/TSS + Heap + preemptive Scheduler OK' in boot else 'FAIL')
    rec(n, 'boot: interrupts', 'PASS' if 'Interrupts OK' in boot else 'FAIL')
    active = 'APIC: LAPIC timer + I/O APIC active' in boot
    if cfg['apic']:
        if active:
            rec(n, 'boot: APIC mode', 'PASS', 'LAPIC timer + I/O APIC active')
        else:
            why = ' | '.join(l.strip() for l in boot.splitlines() if '[apic]' in l) or 'no reason line'
            rec(n, 'boot: APIC mode', 'WARN', 'fell back to legacy PIC: ' + why)
    else:
        rec(n, 'boot: APIC disabled build', 'PASS' if 'APIC not used' in boot else 'FAIL')
    if cfg['smp']:
        m = re.search(r'application processors started: (\d+)', boot)
        want = cfg['cpus'] - 1
        if not m:
            rec(n, 'boot: SMP bring-up', 'FAIL', 'no "[smp]" result line' + (' (APIC fell back?)' if not active else ''))
        else:
            got = int(m.group(1))
            rec(n, 'boot: SMP bring-up', 'PASS' if got == want else 'FAIL', '%d of %d application processors started' % (got, want))


def test_basic(cfg, con):
    n = cfg['name']
    out, ok = con.run('uptime')
    t1 = re.search(r'up \d+h \d+m (\d+)s', out)
    time.sleep(3.2)
    out2, ok2 = con.run('uptime')
    t2 = re.search(r'up \d+h \d+m (\d+)s', out2)
    if t1 and t2:
        adv = (int(t2.group(1)) - int(t1.group(1))) % 60
        rec(n, 'timer advances (uptime)', 'PASS' if 2 <= adv <= 6 else 'FAIL', '%ss over a 3.2 s wait' % adv)
    else:
        rec(n, 'timer advances (uptime)', 'FAIL', 'could not parse uptime: ' + out.strip()[:80])
    out, _ = con.run('tasks')
    rec(n, 'tasks lists kmain+idle0', 'PASS' if 'kmain' in out and 'idle0' in out else 'FAIL', summary_line(out.split('threads:')[-1] and 'threads:' + out.split('threads:')[-1]))
    out, _ = con.run('apic')
    if cfg['apic']:
        rec(n, 'apic command', 'PASS' if has(out, r'APIC: (active|present but NOT active)') else 'FAIL', (re.search(r'APIC: [^\r\n]*', out) or [''])[0])
    else:
        rec(n, 'apic command', 'PASS' if 'not used' in out else 'FAIL')
    out, _ = con.run('cpus')
    m = re.search(r'CPUs online: (\d+)', out)
    got = int(m.group(1)) if m else -1
    want = cfg['cpus'] if cfg['smp'] else 1
    rec(n, 'cpus online', 'PASS' if got == want else 'FAIL', '%d online, expected %d' % (got, want))
    out, _ = con.run('write selftest.txt hello-from-selftest')
    out, _ = con.run('cat selftest.txt')
    rec(n, 'filesystem write/read', 'PASS' if 'hello-from-selftest' in out else 'WARN', 'no persistent disk?' if 'hello-from-selftest' not in out else '')


def test_sched(cfg, con):
    n = cfg['name']
    out, ok = con.run('schedtest 3', timeout=40)
    if not ok:
        rec(n, 'schedtest (preemption)', 'FAIL', 'shell did not come back: ' + out[-120:].replace('\n', ' '))
        return
    nums = {k: int(v) for k, v in re.findall(r'(spin-a|spin-b|spin-low)\s+\(prio \d\):\s+(\d+)', out)}
    if len(nums) != 3:
        rec(n, 'schedtest (preemption)', 'FAIL', 'unparsable: ' + out[-160:].replace('\n', ' '))
        return
    a, b, low = nums['spin-a'], nums['spin-b'], nums['spin-low']
    rec(n, 'busy threads all ran', 'PASS' if min(a, b, low) > 0 else 'FAIL', 'a=%d b=%d low=%d' % (a, b, low))
    ratio = abs(a - b) / max(a, b, 1)
    rec(n, 'equal priorities fair', 'PASS' if ratio < 0.30 else 'FAIL', 'difference %.0f%%' % (ratio * 100))
    rec(n, 'low priority gets less', 'PASS' if low < min(a, b) else 'FAIL')
    rec(n, 'shell responsive while spinning', 'PASS', 'prompt returned after the test')
    out, _ = con.run('tasks')
    time.sleep(1.0)
    out, _ = con.run('tasks')
    left = re.search(r'threads: (\d+)', out)
    rec(n, 'threads cleaned up', 'PASS' if left and int(left.group(1)) == 1 else 'FAIL', 'threads: %s (expected 1)' % (left.group(1) if left else '?'))


def test_user(cfg, con):
    n = cfg['name']
    out, ok = con.run('usertest', timeout=60)
    if not ok:
        rec(n, 'usertest', 'FAIL', 'shell did not come back: ' + out[-160:].replace('\n', ' '))
        return
    rec(n, 'ring 3: hello (syscalls)', 'PASS' if 'Hello from ring 3' in out else 'FAIL')
    rec(n, 'ring 3: heap (brk) + mmap', 'PASS' if '(ok)' in out and 'mmap ok' in out else 'FAIL', re.search(r'heap checksum = \d+ \S+', out).group(0) if has(out, r'heap checksum') else 'no output')
    rec(n, 'ring 3: sleep + preempted loop', 'PASS' if 'counter: done after a preempted busy loop' in out else 'FAIL')
    rec(n, 'ring 3: exit code returned', 'PASS' if '[exit code 7]' in out else 'FAIL')
    rec(n, 'ring 3: fault kills process only', 'PASS' if 'killed by exception' in out and 'NOT REACHED' not in out and 'usertest done' in out else 'FAIL')
    out, ok = con.run('exec hello.elf a bb ccc', timeout=20)
    rec(n, 'exec with arguments', 'PASS' if has(out, r'argc=4') and 'ccc' in out else 'FAIL', re.search(r'argc=\d+[^\r\n]*', out).group(0)[:60] if has(out, r'argc=') else 'no output')
    out, ok = con.run('exec fsio.elf', timeout=20)
    rec(n, 'user file syscalls', 'PASS' if 'fsio: ok' in out else ('WARN' if 'skipped' in out else 'FAIL'), re.search(r'fsio: [^\n\r]*', out).group(0) if 'fsio:' in out else 'no output')
    # background run + ps + kill
    out, _ = con.run('run counter.elf')
    m = re.search(r'started pid (\d+)', out)
    if m:
        pid = m.group(1)
        out, _ = con.run('ps')
        rec(n, 'run (background) + ps', 'PASS' if 'counter.elf' in out else 'FAIL')
        out, _ = con.run('kill ' + pid)
        time.sleep(1.0)
        out, _ = con.run('ps')
        row = re.search(r'^\s*' + pid + r'\s[^\r\n]*', out, re.M)
        rec(n, 'kill a background process', 'PASS' if row and 'exited' in row.group(0) else 'FAIL', row.group(0).strip()[:80] if row else 'pid not listed')
    else:
        rec(n, 'run (background) + ps', 'FAIL', out.strip()[-100:])
    # Ctrl-C on a foreground program
    con.send('exec counter.elf\r')
    time.sleep(0.5)
    con.send(b'\x03')
    out, ok = con.read_until_prompt(15)
    rec(n, 'Ctrl-C kills foreground program', 'PASS' if ok and 'exit code -9' in out else 'FAIL', 'exit code -9 seen' if 'exit code -9' in out else out[-100:].replace('\n', ' '))


def run_config(cfg, args):
    n = cfg['name']
    print('\n=== %s  (HH=%d SMP=%d APIC=%d, %d CPU)' % (n, cfg['hh'], cfg['smp'], cfg['apic'], cfg['cpus']), flush=True)
    disk = os.path.join(REPORT, 'disk_%s.img' % n)
    with open(disk, 'wb') as f:
        f.truncate(16 * 1024 * 1024)
    con = Console(qemu_argv(cfg, disk))
    try:
        boot, ok = con.read_until_prompt(args.boot_timeout)
        if not ok:
            why = 'QEMU exited (crash / triple fault / reboot loop)' if not con.alive() else 'no shell prompt within %ds (hang)' % args.boot_timeout
            tail = '\n'.join(boot.replace('\r', '').splitlines()[-12:])
            rec(n, 'boot reaches shell prompt', 'FAIL', why + '\nlast serial output:\n' + tail)
            test_boot(cfg, boot)
            return
        rec(n, 'boot reaches shell prompt', 'PASS')
        test_boot(cfg, boot)
        test_basic(cfg, con)
        out0, _ = con.run('mem')
        f0 = frames(out0)
        test_sched(cfg, con)
        if cfg['full'] or True:
            test_user(cfg, con)
        out1, _ = con.run('mem')
        f1 = frames(out1)
        if f0 is not None and f1 is not None:
            drift = f0 - f1
            rec(n, 'no memory leak after tests', 'PASS' if drift <= 48 else 'FAIL', '%d frames lower than before (limit 48: page-table/guard splits)' % drift)
        out, ok = con.run('uptime')
        rec(n, 'system still alive at the end', 'PASS' if ok else 'FAIL')
    finally:
        with open(os.path.join(REPORT, 'serial_%s.log' % n), 'w') as f:
            f.write(con.all.decode(errors='replace').replace('\r', ''))
        con.close()


# ------------------------------------------------------------------ report
def write_report(args, started):
    counts = {k: sum(1 for r in results if r[2] == k) for k in ('PASS', 'FAIL', 'WARN', 'SKIP')}
    names = []
    for r in results:
        if r[0] not in names:
            names.append(r[0])
    lines = ['# AOS self-test report', '', 'Generated %s, took %ds' % (datetime.datetime.now().isoformat(timespec='seconds'), time.time() - started),
             'Host: %s, %s, KVM: %s' % (platform.platform(), platform.machine(), 'yes' if os.access('/dev/kvm', os.W_OK) else 'no (QEMU TCG)'),
             '', '**%d passed, %d failed, %d warnings**' % (counts['PASS'], counts['FAIL'], counts['WARN']), '']
    if counts['FAIL']:
        lines += ['## Failures', '']
        for c, nme, st, d in results:
            if st == 'FAIL':
                lines.append('* **%s / %s**: %s' % (c, nme, d.replace('\n', '\n    ')))
        lines.append('')
    if counts['WARN']:
        lines += ['## Warnings', '']
        for c, nme, st, d in results:
            if st == 'WARN':
                lines.append('* %s / %s: %s' % (c, nme, d))
        lines.append('')
    lines += ['## All results', '']
    for n in names:
        lines += ['### ' + n, '', '| check | result | detail |', '|---|---|---|']
        for c, nme, st, d in results:
            if c == n:
                lines.append('| %s | %s | %s |' % (nme, st, d.split('\n')[0].replace('|', '/')))
        lines.append('')
    lines += ['Logs: serial_<config>.log (everything the kernel printed), build_<config>.log, host_tests.log.']
    path = os.path.join(REPORT, 'REPORT.md')
    open(path, 'w').write('\n'.join(lines) + '\n')
    zp = os.path.join(ROOT, 'selftest_report.zip')
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(REPORT)):
            if not f.endswith('.img'):
                z.write(os.path.join(REPORT, f), 'selftest_report/' + f)
    return counts, path, zp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--quick', action='store_true', help='default configuration only')
    ap.add_argument('--no-install', action='store_true')
    ap.add_argument('--boot-timeout', type=int, default=45)
    ap.add_argument('--fake', action='store_true', help='harness self-test: use tools/fake_qemu.py instead of QEMU')
    args = ap.parse_args()
    started = time.time()
    shutil.rmtree(REPORT, ignore_errors=True)
    os.makedirs(REPORT)
    if args.fake:
        os.environ['AOS_SELFTEST_QEMU'] = sys.executable + ' ' + os.path.join(ROOT, 'tools', 'fake_qemu.py')
    if not ensure_tools(args):
        print('cannot continue without the build/emulation tools')
        return 2
    print('=== host unit tests + static checks')
    if args.fake:
        rec('host', 'unit tests', 'SKIP', 'fake run')
    else:
        rc, out = sh(['make', '-C', 'tests/host', 'run'], log='host_tests.log')
        m = re.search(r'(\d+) checks, (\d+) failed', out)
        rec('host', 'unit tests (scheduler/MADT/ELF/aspace)', 'PASS' if rc == 0 and m else 'FAIL', m.group(0) if m else out[-200:])
    for cfg in (CONFIGS[:1] if args.quick else CONFIGS):
        if build(cfg, args):
            run_config(cfg, args)
    counts, path, zp = write_report(args, started)
    print('\n==============================================================')
    print('RESULT: %d passed, %d failed, %d warnings' % (counts['PASS'], counts['FAIL'], counts['WARN']))
    print('Report : ' + path)
    print('Upload : ' + zp + '   <- send this file (or paste REPORT.md)')
    return 1 if counts['FAIL'] else 0


if __name__ == '__main__':
    sys.exit(main())
