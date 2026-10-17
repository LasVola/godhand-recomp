"""Disassembles VU microcode (upper + lower pairs).

Sources: a raw file (--raw), or the ELF with --elf-off pointing at the first code word after a VIF MPG command.
Usage:
  python tools/vu_disasm.py --elf build/SLUS_212.09 --elf-off 0x24bf00 --start 0xf0 [--count 40] [--until-e]
"""
import argparse
import struct

DEST = lambda d: "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if d & b)
BC = "xyzw"

UPPER = {}
for base, name in ((0x00, "ADD"), (0x04, "SUB"), (0x08, "MADD"), (0x0C, "MSUB"), (0x10, "MAX"), (0x14, "MINI"), (0x18, "MUL")):
    for i in range(4):
        UPPER[base + i] = (name, "bc", i)
UPPER.update({0x1C: ("MULq", "q", 0), 0x1D: ("MAXi", "i", 0), 0x1E: ("MULi", "i", 0), 0x1F: ("MINIi", "i", 0),
              0x20: ("ADDq", "q", 0), 0x21: ("MADDq", "q", 0), 0x22: ("ADDi", "i", 0), 0x23: ("MADDi", "i", 0),
              0x24: ("SUBq", "q", 0), 0x25: ("MSUBq", "q", 0), 0x26: ("SUBi", "i", 0), 0x27: ("MSUBi", "i", 0),
              0x28: ("ADD", "3", 0), 0x29: ("MADD", "3", 0), 0x2A: ("MUL", "3", 0), 0x2B: ("MAX", "3", 0),
              0x2C: ("SUB", "3", 0), 0x2D: ("MSUB", "3", 0), 0x2E: ("OPMSUB", "3", 0), 0x2F: ("MINI", "3", 0)})
UPPER_EXT = {}
for base, name in ((0x00, "ADDA"), (0x04, "SUBA"), (0x08, "MADDA"), (0x0C, "MSUBA"), (0x18, "MULA")):
    for i in range(4):
        UPPER_EXT[base + i] = (name, "abc", i)
for i, n in enumerate(("ITOF0", "ITOF4", "ITOF12", "ITOF15")):
    UPPER_EXT[0x10 + i] = (n, "2", 0)
for i, n in enumerate(("FTOI0", "FTOI4", "FTOI12", "FTOI15")):
    UPPER_EXT[0x14 + i] = (n, "2", 0)
UPPER_EXT.update({0x1C: ("MULAq", "aq", 0), 0x1D: ("ABS", "2", 0), 0x1E: ("MULAi", "ai", 0), 0x1F: ("CLIPw", "clip", 0),
                  0x20: ("ADDAq", "aq", 0), 0x21: ("MADDAq", "aq", 0), 0x22: ("ADDAi", "ai", 0), 0x23: ("MADDAi", "ai", 0),
                  0x24: ("SUBAq", "aq", 0), 0x25: ("MSUBAq", "aq", 0), 0x26: ("SUBAi", "ai", 0), 0x27: ("MSUBAi", "ai", 0),
                  0x28: ("ADDA", "a3", 0), 0x29: ("MADDA", "a3", 0), 0x2A: ("MULA", "a3", 0), 0x2C: ("SUBA", "a3", 0),
                  0x2D: ("MSUBA", "a3", 0), 0x2E: ("OPMULA", "a3", 0), 0x2F: ("NOP", "nop", 0)})


def upper(w):
    flags = "".join(f for f, b in (("I", 31), ("E", 30), ("M", 29), ("D", 28), ("T", 27)) if (w >> b) & 1)
    dest, ft, fs, fd, op = (w >> 21) & 15, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    if op >= 0x3C:
        ext = ((w >> 6) & 31) << 2 | (w & 3)
        name, kind, bc = UPPER_EXT.get(ext, ("?ext%02x" % ext, "nop", 0))
    else:
        name, kind, bc = UPPER.get(op, ("?%02x" % op, "nop", 0))
    d = DEST(dest)
    if kind == "bc":
        s = f"{name}{BC[bc]}.{d} vf{fd}, vf{fs}, vf{ft}{BC[bc]}"
    elif kind in ("q", "i"):
        s = f"{name}.{d} vf{fd}, vf{fs}, {kind.upper()}"
    elif kind == "3":
        s = f"{name}.{d} vf{fd}, vf{fs}, vf{ft}"
    elif kind == "abc":
        s = f"{name}{BC[bc]}.{d} ACC, vf{fs}, vf{ft}{BC[bc]}"
    elif kind in ("aq", "ai"):
        s = f"{name}.{d} ACC, vf{fs}, {kind[1].upper()}"
    elif kind == "a3":
        s = f"{name}.{d} ACC, vf{fs}, vf{ft}"
    elif kind == "2":
        s = f"{name}.{d} vf{ft}, vf{fs}"
    elif kind == "clip":
        s = f"CLIPw.xyz vf{fs}, vf{ft}w"
    else:
        s = name
    return s, flags


LOWER1 = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
LOWER_EXT = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV", 0x39: "SQRT",
             0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR", 0x40: "RNEXT",
             0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP", 0x6C: "XGKICK",
             0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy", 0x75: "EATANxz",
             0x76: "ESUM", 0x78: "ESQRT", 0x79: "ERSQRT", 0x7A: "ERCPR", 0x7B: "WAITP", 0x7C: "ESIN", 0x7D: "EATAN",
             0x7E: "EEXP"}
LOWER2 = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ", 0x11: "FCSET",
          0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ",
          0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR", 0x28: "IBEQ",
          0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lower(w, pc):
    dest, ft, fs, fd = (w >> 21) & 15, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    d = DEST(dest)
    if w >> 31:
        op = w & 63
        if op >= 0x3C:
            ext = ((w >> 6) & 31) << 2 | (w & 3)
            name = LOWER_EXT.get(ext, "?lext%02x" % ext)
            fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
            if name in ("DIV", "RSQRT"):
                return f"{name} Q, vf{fs}{BC[fsf]}, vf{ft}{BC[ftf]}"
            if name == "SQRT":
                return f"SQRT Q, vf{ft}{BC[ftf]}"
            if name in ("MOVE", "MR32"):
                return f"{name}.{d} vf{ft}, vf{fs}"
            if name in ("MTIR",):
                return f"MTIR vi{ft}, vf{fs}{BC[fsf]}"
            if name in ("MFIR",):
                return f"MFIR.{d} vf{ft}, vi{fs}"
            if name in ("LQI", "LQD"):
                return f"{name}.{d} vf{ft}, (vi{fs}{'++' if name == 'LQI' else '--'})"
            if name in ("SQI", "SQD"):
                return f"{name}.{d} vf{fs}, (vi{ft}{'++' if name == 'SQI' else '--'})"
            if name in ("ILWR", "ISWR"):
                return f"{name}.{d} vi{ft}, (vi{fs})"
            if name.startswith("E") or name in ("WAITQ", "WAITP", "MFP", "XGKICK", "XTOP", "XITOP"):
                return f"{name} vf{fs}{BC[fsf]} vi{ft}" if name.startswith("E") else f"{name} vi{fs}/vi{ft}"
            return f"{name} {d} vf{fs} vf{ft}"
        name = LOWER1.get(op, "?l1_%02x" % op)
        if name == "IADDI":
            return f"IADDI vi{ft}, vi{fs}, {sext((w >> 6) & 31, 5)}"
        return f"{name} vi{fd}, vi{fs}, vi{ft}"
    op = (w >> 25) & 127
    name = LOWER2.get(op, "?l2_%02x" % op)
    imm11 = sext(w & 0x7FF, 11)
    if name in ("LQ", "SQ", "ILW", "ISW"):
        return f"{name}.{d} {'vf' if name in ('LQ','SQ') else 'vi'}{ft if name in ('LQ','ILW') else fs}, {imm11}(vi{fs if name in ('LQ','ILW') else ft})"
    if name in ("IADDIU", "ISUBIU"):
        imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
        return f"{name} vi{ft}, vi{fs}, {imm15}"
    if name in ("B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        target = (pc + 8 + imm11 * 8) & 0x3FFF
        return f"{name} vi{ft}, vi{fs}, 0x{target:x}"
    if name in ("JR", "JALR"):
        return f"{name} vi{ft}, vi{fs}"
    return f"{name} 0x{w & 0xFFFFFF:x}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf")
    ap.add_argument("--elf-off", type=lambda v: int(v, 0))
    ap.add_argument("--raw")
    ap.add_argument("--start", type=lambda v: int(v, 0), default=0)
    ap.add_argument("--count", type=int, default=64)
    ap.add_argument("--until-e", action="store_true")
    a = ap.parse_args()
    data = open(a.elf or a.raw, "rb").read()
    base = a.elf_off or 0
    e_seen = -1
    for i in range(a.count):
        pc = a.start + i * 8
        lo, up = struct.unpack_from("<II", data, base + pc)
        us, flags = upper(up)
        if (up >> 31) & 1:
            ls = f"LOI {struct.unpack('<f', struct.pack('<I', lo))[0]:.7g} (0x{lo:08x})"
        else:
            ls = lower(lo, pc)
        print(f"{pc:04x}: {us:<38} {ls:<44} {flags}")
        if "E" in flags:
            e_seen = i
        if a.until_e and e_seen >= 0 and i == e_seen + 1:
            break


if __name__ == "__main__":
    main()
