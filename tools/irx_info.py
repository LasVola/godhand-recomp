"""irx_info.py <module.irx> [--base HEX] [--dis START END]

Prints an IOP module's import stubs (library, function number, stub address) and optionally disassembles a range.
Addresses are module-relative unless --base gives the load address (e.g. the "loaded IRX ... base=" log line).
"""
import argparse
import struct

import capstone


def load(path, base=0):
    """The module image as loaded at `base`, with its MIPS relocations (.rel sections) applied."""
    d = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', d, 0x1C)[0]
    phnum = struct.unpack_from('<H', d, 0x2C)[0]
    img = None
    for i in range(phnum):
        t, off, va, pa, fsz, msz = struct.unpack_from('<IIIIII', d, phoff + i * 32)
        if t == 1:  # the loadable image
            img = bytearray(d[off:off + fsz] + bytes(msz - fsz))
            break
    if img is None:
        raise SystemExit('no PT_LOAD')
    shoff = struct.unpack_from('<I', d, 0x20)[0]
    shnum = struct.unpack_from('<H', d, 0x30)[0]
    for i in range(shnum):
        name, stype, flags, addr, off, size = struct.unpack_from('<IIIIII', d, shoff + i * 40)
        if stype != 9:  # SHT_REL
            continue
        rels = [struct.unpack_from('<II', d, off + j) for j in range(0, size, 8)]
        hi_pending = []
        for r_off, info in rels:
            rtype = info & 0xFF
            if r_off + 4 > len(img):
                continue
            word = struct.unpack_from('<I', img, r_off)[0]
            if rtype == 2:  # R_MIPS_32
                word = (word + base) & 0xFFFFFFFF
            elif rtype == 4:  # R_MIPS_26
                target = ((word & 0x3FFFFFF) << 2) + base
                word = (word & 0xFC000000) | ((target >> 2) & 0x3FFFFFF)
            elif rtype == 5:  # R_MIPS_HI16: resolved with the next LO16
                hi_pending.append(r_off)
                continue
            elif rtype == 6:  # R_MIPS_LO16
                lo = word & 0xFFFF
                lo_s = lo - 0x10000 if lo & 0x8000 else lo
                for h in hi_pending:
                    hw = struct.unpack_from('<I', img, h)[0]
                    full = ((hw & 0xFFFF) << 16) + lo_s + base
                    hw = (hw & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF)
                    struct.pack_into('<I', img, h, hw)
                hi_pending = []
                word = (word & 0xFFFF0000) | ((lo + base) & 0xFFFF)
            struct.pack_into('<I', img, r_off, word)
    return bytes(img)


def imports(img):
    out = []
    pos = 0
    while True:
        pos = img.find(b'\x00\x00\xe0\x41', pos)
        if pos < 0 or pos % 4:
            if pos < 0:
                break
            pos += 1
            continue
        name = img[pos + 12:pos + 20].split(b'\0')[0].decode('latin1')
        p = pos + 20
        while p + 8 <= len(img):
            a, b = struct.unpack_from('<II', img, p)
            if a != 0x03E00008 or (b >> 16) != 0x2400:
                break
            out.append((name, b & 0xFFFF, p))
            p += 8
        pos = p
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('irx')
    ap.add_argument('--base', default='0')
    ap.add_argument('--dis', nargs=2)
    a = ap.parse_args()
    base = int(a.base, 16)
    img = load(a.irx, base)
    for lib, num, addr in imports(img):
        print(f'import {lib:10s} #{num:3d} stub 0x{addr + base:06x}')
    if a.dis:
        s, e = int(a.dis[0], 16) - base, int(a.dis[1], 16) - base
        md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 | capstone.CS_MODE_LITTLE_ENDIAN)
        md.skipdata = True
        for ins in md.disasm(img[s:e], s + base):
            print(f'{ins.address:06x}: {ins.mnemonic:8} {ins.op_str}')


if __name__ == '__main__':
    main()
