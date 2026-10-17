"""Minimal MIPS ELF writer for ps2_recomp input (port/runtime/ps2xRecomp/src/lib/elf_parser.cpp).

What the fork's ElfParser needs (elf_parser.cpp, Oct 17 2026 tree):
  - ELFIO must load the file; e_machine == EM_MIPS (parse(), line 2409), otherwise "Not a MIPS ELF file".
  - Section headers drive everything when present (loadSections(), 2426-2469): code = SHF_EXECINSTR and a name other
    than .vutext*/.DVP.overlay* (isExecutableSection(), 1788); data = SHF_ALLOC without SHF_EXECINSTR (1803);
    section bytes come from sh_offset/sh_size; SHT_NOBITS has no data. Only when there are NO section headers are
    PT_LOAD segments turned into sections (AppendLoadSegmentsAsSections, 37-104).
  - Function-map rows (loadGhidraFunctionMap(), 2230) are kept only if Start lies in an executable section; End is
    clamped to that section. readWord() (2108) reads from any section with data.
  - No symbol table, relocation section or DWARF is required (loadSymbols/loadRelocations just find none).
Layout written: ELF header, one PT_LOAD (RWX) mapping the whole module image at `base`, image bytes, then
.shstrtab and the section header table. e_entry = the module's ctor routine (a real code address).
"""
from __future__ import annotations

import struct

EM_MIPS = 8
ET_EXEC = 2
PT_LOAD = 1
SHT_PROGBITS = 1
SHT_STRTAB = 3
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
EF_BOOT = 0x20924001  # e_flags of SLUS_215.03 (copied so the module looks like the boot ELF to tools)
IMG_OFF = 0x100


def _flags(s):
    return (SHF_WRITE if "W" in s else 0) | (SHF_ALLOC if "A" in s else 0) | (SHF_EXECINSTR if "X" in s else 0)


def build_exec_elf(img: bytes, base: int, entry: int, sections):
    """sections: [(name, start_off, end_off, flags 'A'/'AX'/'WA')], offsets relative to the image start."""
    img_off = IMG_OFF + (base & 0x7F)          # p_offset == p_vaddr (mod p_align)
    shstr = b"\0"
    name_off = []
    for nm, *_ in sections:
        name_off.append(len(shstr))
        shstr += nm.encode() + b"\0"
    shstr_name = len(shstr)
    shstr += b".shstrtab\0"
    shstr_off = img_off + len(img)
    shoff = (shstr_off + len(shstr) + 3) & ~3
    shnum = len(sections) + 2
    eh = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    eh += struct.pack("<HHIIIIIHHHHHH", ET_EXEC, EM_MIPS, 1, entry, 52, shoff, EF_BOOT, 52, 32, 1, 40, shnum,
                      shnum - 1)
    ph = struct.pack("<8I", PT_LOAD, img_off, base, base, len(img), len(img), 7, 0x80)
    out = bytearray(eh + ph)
    out += bytes(img_off - len(out))
    out += img
    out += shstr
    out += bytes(shoff - len(out))
    sh = [struct.pack("<10I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for (nm, a, b, fl), no in zip(sections, name_off):
        align = 4 if "X" in fl else 1
        sh.append(struct.pack("<10I", no, SHT_PROGBITS, _flags(fl), base + a, img_off + a, b - a, 0, 0, align, 0))
    sh.append(struct.pack("<10I", shstr_name, SHT_STRTAB, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    for s in sh:
        out += s
    return bytes(out)


def read_elf(elf: bytes):
    """Tiny reader mirroring what ElfParser uses: machine, entry, PT_LOADs, sections (name, addr, size, flags, data)."""
    assert elf[:4] == b"\x7fELF" and elf[4] == 1 and elf[5] == 1, "not ELF32 LSB"
    (e_type, e_machine, _v, e_entry, e_phoff, e_shoff, e_flags, _eh, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
    phs = [struct.unpack_from("<8I", elf, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    shs = [struct.unpack_from("<10I", elf, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    strtab = shs[e_shstrndx]
    names = elf[strtab[4]:strtab[4] + strtab[5]]
    secs = []
    for s in shs:
        nm = names[s[0]:names.index(b"\0", s[0])].decode()
        secs.append({"name": nm, "type": s[1], "flags": s[2], "addr": s[3], "offset": s[4], "size": s[5],
                     "data": elf[s[4]:s[4] + s[5]] if s[1] not in (0, 8) else None})
    return {"type": e_type, "machine": e_machine, "entry": e_entry, "flags": e_flags, "phdrs": phs, "sections": secs}


def self_check(elf: bytes, img: bytes, base: int, sections):
    e = read_elf(elf)
    assert e["machine"] == EM_MIPS and e["type"] == ET_EXEC
    loads = [p for p in e["phdrs"] if p[0] == PT_LOAD]
    assert len(loads) == 1 and loads[0][2] == base and loads[0][4] == len(img)
    assert elf[loads[0][1]:loads[0][1] + len(img)] == img
    code = [s for s in e["sections"] if s["flags"] & SHF_EXECINSTR]
    assert len(code) == 1 and code[0]["name"] == ".text"
    for (nm, a, b, _fl) in sections:
        s = next(x for x in e["sections"] if x["name"] == nm)
        assert s["addr"] == base + a and s["size"] == b - a and s["data"] == img[a:b], nm
    lo = base
    for s in sorted((x for x in e["sections"] if x["flags"] & SHF_ALLOC), key=lambda x: x["addr"]):
        assert s["addr"] == lo, ("hole/overlap at", hex(lo), s["name"])
        lo = s["addr"] + s["size"]
    assert lo == base + len(img)
    assert code[0]["addr"] <= e["entry"] < code[0]["addr"] + code[0]["size"], "entry outside .text"
    return True
