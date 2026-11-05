#!/usr/bin/env python3
"""Static sanity checks for kernel.elf (no emulator needed): Multiboot2 header, entry point, load layout."""
import struct, sys

path = sys.argv[1]
hh = len(sys.argv) > 2 and sys.argv[2] == '1'
d = open(path, 'rb').read()

i = d.find(struct.pack('<I', 0xe85250d6))
assert 0 <= i < 32768, 'Multiboot2 header must be in the first 32 KiB (found at %d)' % i
magic, arch, length, checksum = struct.unpack_from('<IIII', d, i)
assert (magic + arch + length + checksum) & 0xffffffff == 0, 'bad Multiboot2 checksum'

assert d[:4] == b'\x7fELF' and d[4] == 2, 'not an ELF64 file'
e_entry, e_phoff = struct.unpack_from('<QQ', d, 24)
e_phentsize, e_phnum = struct.unpack_from('<HH', d, 54)
assert e_entry < 0x100000000, 'ELF entry must be a low (physical) address, GRUB jumps to it in 32-bit mode'
loads = []
for k in range(e_phnum):
    p_type, p_flags, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', d, e_phoff + k * e_phentsize)
    if p_type == 1:
        loads.append((p_vaddr, p_paddr, p_filesz, p_memsz))
assert loads, 'no PT_LOAD segments'
entry_ok = False
for vaddr, paddr, filesz, memsz in loads:
    assert paddr < 0x100000000, 'segment loads above 4 GiB: %#x' % paddr
    if vaddr >= 0xFFFFFFFF80000000:
        assert vaddr - 0xFFFFFFFF80000000 == paddr, 'higher-half segment must map phys = virt - 0xFFFFFFFF80000000'
    if vaddr <= e_entry < vaddr + memsz and vaddr == paddr:
        entry_ok = True
assert entry_ok, 'entry point is not inside a segment with vaddr == paddr'
if hh:
    assert any(v >= 0xFFFFFFFF80000000 for v, *_ in loads), 'HH=1 but no higher-half segment'
print('Multiboot2 header offset:', i)
print('ELF entry: %#x' % e_entry)
for v, p, fs, ms in loads:
    print('  LOAD vaddr=%#x paddr=%#x filesz=%#x memsz=%#x' % (v, p, fs, ms))
print('ELF size:', len(d))
print('TEST OK')
