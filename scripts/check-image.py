#!/usr/bin/env python3
"""Check the two ELF/linker regressions found during CH32V208 bring-up.

Usage: scripts/check-image.py build/ch32v208_nimble.elf [build/boot.elf]
Uses only the Python standard library; expects the sibling .map file.
"""
import re
import struct
import sys
from pathlib import Path


def check(path):
    data = path.read_bytes()
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected a little-endian ELF32 image')
    hdr = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    phoff, shoff, phsize, phnum, shsize, shnum, shstridx = (
        hdr[5], hdr[6], hdr[9], hdr[10], hdr[11], hdr[12], hdr[13]
    )
    sections = [struct.unpack_from('<10I', data, shoff + i * shsize)
                for i in range(shnum)]
    strings = sections[shstridx]
    shnames = data[strings[4]:strings[4] + strings[5]]

    def cstr(buf, off):
        return buf[off:buf.index(0, off)].decode()

    named = {cstr(shnames, s[0]): s for s in sections}
    symbols = {}
    for s in sections:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        names = sections[s[6]]
        names = data[names[4]:names[4] + names[5]]
        for off in range(s[4], s[4] + s[5], s[9]):
            name, value, _, _, _, _ = struct.unpack_from('<IIIBBH', data, off)
            symbols[cstr(names, name)] = value

    errors = []
    ram_data = named['.data']
    lmas = []
    for i in range(phnum):
        p = struct.unpack_from('<8I', data, phoff + i * phsize)
        if p[0] == 1 and p[1] <= ram_data[4] and (
                ram_data[4] + ram_data[5] <= p[1] + p[4]):
            lmas.append(p[3] + ram_data[4] - p[1])
    if len(lmas) != 1:
        errors.append(f'.data must have one load segment, found {lmas}')
    else:
        lma = lmas[0]
        if symbols.get('_data_lma') != lma:
            errors.append(f'_data_lma={symbols.get("_data_lma"):#x}, actual LMA={lma:#x}')
        if lma % 4:
            errors.append(f'.data LMA is not word aligned: {lma:#x}')
    if symbols.get('_data_vma') != ram_data[3]:
        errors.append('_data_vma does not match .data')
    if symbols.get('_edata') != ram_data[3] + ram_data[5]:
        errors.append('_edata does not match .data end')
    if ram_data[5] % 4:
        errors.append('.data size must be a multiple of the CRT copy word size')
    if not symbols['_heap_end'] <= symbols['_susrstack']:
        errors.append('newlib heap overlaps reset stack')

    link_map = path.with_suffix('.map').read_text()
    for sym in ('_sbrk', '_sbrk_r', '__malloc_lock', '__malloc_unlock'):
        pattern = (r'\.text\.' + re.escape(sym) +
                   r'\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+[^\n]*port/syscalls\.c\.obj')
        if not re.search(pattern, link_map):
            errors.append(f'{sym} must come from port/syscalls.c')
    if errors:
        print(f'FAIL {path}:\n  ' + '\n  '.join(errors))
        return False
    print(f'PASS {path}: .data LMA={lmas[0]:#x}, {ram_data[5]} bytes; syscall overrides linked')
    return True


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    results = [check(Path(arg)) for arg in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)
