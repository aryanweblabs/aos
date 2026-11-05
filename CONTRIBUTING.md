# Contributing to AOS

Thanks for your interest in improving AOS. This project is maintained by
**[aryanweblabs](https://github.com/aryanweblabs)** and is open source under the MIT License.

## Development setup

```bash
sudo apt install build-essential grub-pc-bin grub-common xorriso mtools qemu-system-x86
git clone https://github.com/aryanweblabs/aos.git
cd aos
make
make run-serial
```

See [docs/USAGE.md](docs/USAGE.md) for full instructions.

## Code guidelines

- Prefer clear, small commits with a short why-message
- Match existing style in the file you edit (C++17 freestanding kernel, no RTTI/exceptions)
- Kernel code: no dynamic C++ features that need libstdc++
- New syscalls: update `src/process/syscall.hpp`, `syscall.cpp`, `user/libaos.h`, and `docs/SYSCALLS.md`
- Memory/FS changes: update `docs/MEMORY.md` or `docs/VFS.md` when behaviour changes

## Testing

Before opening a PR:

```bash
make check          # static ELF checks + host unit tests (where available)
make run-serial     # manual smoke: boot, help, mem, exec vmtest.elf
```

Useful in-QEMU checks:

```text
exec vmtest.elf
exec forkdemo.elf
write t.txt hello
cat t.txt
cache
```

## Pull requests

1. Fork the repo and create a branch (`feature/...` or `fix/...`)
2. Keep the PR focused (one topic)
3. Describe what changed and how you tested it
4. Do not commit `build/`, ISOs, or disk images

## Reporting issues

Include:

- Host OS and QEMU version
- Exact command you ran
- Serial log excerpt (boot + failure)
- Whether you used `HH=1`, `SMP=1`, or `APIC=0`

## License

By contributing, you agree that your contributions are licensed under the same
MIT License that covers this repository.
