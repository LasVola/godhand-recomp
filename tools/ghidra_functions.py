"""Builds recomp/urban_functions.csv, the Ghidra function map that recomp/urban.toml feeds to ps2_recomp.

1. Imports and auto-analyzes build/SLUS_212.09 in a headless Ghidra project (language r5900:LE:32:default from the
   ghidra-emotionengine-reloaded extension). Skip with --no-analyze when the project already exists.
2. Exports functions with PS2Recomp's ExportPS2Functions.java.
3. Keeps only real functions (FUN_*; the export also lists ~33k code labels, which must not become entries) and
   drops starts in a branch delay slot plus known bad splits.

Usage: python tools/ghidra_functions.py [--ghidra H:/tools/ghidra_12.1.4_PUBLIC] [--no-analyze]
"""
import argparse
import bisect
import csv
import os
import struct
import subprocess

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
ELF = os.path.join(ROOT, "build", "SLUS_212.09")
OUT = os.path.join(ROOT, "build", "ghidra")
SCRIPTS = os.path.join(ROOT, "runtime", "ps2xRecomp", "tools", "ghidra")
# Ghidra splits that land in the middle of a function (checked by hand).
BAD_STARTS = {0x326230}


def headless(ghidra, *args):
    cmd = [os.path.join(ghidra, "support", "analyzeHeadless.bat"), OUT, "UrbanReign", *args]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def is_control_transfer(word):
    op, rs, rt, fn = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 63
    return (op in (2, 3) or 4 <= op <= 7 or 0x14 <= op <= 0x17 or (op == 1 and (rt <= 3 or 16 <= rt <= 19)) or
            (op == 0 and fn in (8, 9)) or (op in (0x11, 0x12) and rs == 8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ghidra", default=r"H:\tools\ghidra_12.1.4_PUBLIC")
    ap.add_argument("--no-analyze", action="store_true")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    raw_csv = os.path.join(OUT, "ghidra_functions.csv")
    if not args.no_analyze:
        headless(args.ghidra, "-import", ELF, "-processor", "r5900:LE:32:default", "-overwrite",
                 "-analysisTimeoutPerFile", "3600")
    headless(args.ghidra, "-process", os.path.basename(ELF), "-noanalysis", "-readOnly", "-scriptPath", SCRIPTS,
             "-postScript", "ExportPS2Functions.java", os.path.join(OUT, "ghidra_export.toml"), raw_csv)

    elf = open(ELF, "rb").read()
    phoff, phnum = struct.unpack_from("<I", elf, 0x1C)[0], struct.unpack_from("<H", elf, 0x2C)[0]
    segments = [struct.unpack_from("<6I", elf, phoff + i * 32) for i in range(phnum)]

    def word_at(address):
        for _, offset, vaddr, _, filesz, _ in segments:
            if vaddr <= address < vaddr + filesz:
                return struct.unpack_from("<I", elf, offset + address - vaddr)[0]
        return None

    rows = list(csv.reader(open(raw_csv, newline="")))
    def is_dropped(start):
        previous = word_at(start - 4)
        return start in BAD_STARTS or (previous is not None and is_control_transfer(previous))

    starts = sorted(int(row[1], 0) for row in rows[1:] if row[0].startswith("FUN_") and not is_dropped(int(row[1], 0)))
    kept, dropped = [rows[0]], []
    for row in rows[1:]:
        if not row[0].startswith("FUN_"):
            continue
        start = int(row[1], 0)
        if is_dropped(start):
            dropped.append(hex(start))
            continue
        # Ghidra's End is the highest address of the body; a function with a detached chunk (shared tail code far
        # away) would otherwise span ~100 KB and be emitted again for every such function (C1002 in MSVC). Its Size
        # skips holes inside the body (0x3313d8: 972 bytes, but code runs to 0x3317b8), so cut at the next
        # function start instead.
        # Holes after the body (code reached only by jumps, padding) stay with the function, as in the
        # recompiler's own discovery; otherwise they would be covered by nothing.
        end = int(row[2], 0)
        following = bisect.bisect_right(starts, start)
        if following < len(starts):
            next_start = starts[following]
            end = next_start if next_start - start <= 0x20000 else min(end, next_start)
        # Same names as the recompiler's own discovery, so generated file names stay stable.
        kept.append([f"sub_{start:08X}", f"0x{start:08X}", f"0x{end:08X}", str(end - start)])
    out = os.path.join(ROOT, "recomp", "urban_functions.csv")
    csv.writer(open(out, "w", newline="")).writerows(kept)
    print(f"{out}: {len(kept) - 1} functions, dropped {dropped}")


if __name__ == "__main__":
    main()
