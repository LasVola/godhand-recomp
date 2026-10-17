"""Locate Sony SDK functions in the stripped Urban Reign ELF.

Urban Reign ships without symbols. Shadow of the Colossus (SDK 3.0, with symbols) links many of the same library
objects, so its named functions serve as signatures. Urban Reign uses SDK 2.8, so four passes are used:

0. syscall: libkernel syscall wrappers (addiu $v1, N; syscall; jr $ra) are named by syscall number.
1. exact: the normalized instruction sequence (jump targets and 16-bit immediates masked) occurs exactly once.
2. near:  for unmatched functions, look around the address predicted by the nearest matched neighbours (libraries
          keep their object layout) and accept a fuzzy match (>= 80% instruction similarity, same prologue).
3. order: an unmatched run between two matched anchors whose function count equals the reference count is
          assigned in order.
4. rpc:   remaining libcdvd entry points are named by the command they send through sceSifCallRpc on the N-command
          (client used by sceCdRead) or S-command (client used by sceCdGetError) server. Command numbers follow
          libcdvd (same numbering in PS2SDK); sceCdRead = N1 and sceCdGetError = S4 confirm it.

Only SDK names (sce*, _sce*, isce*) are kept: short game functions from the reference produce false positives.

Output: src/urban/sdk_functions.inc  (X(address, name, method, score) rows, consumed by hle_bindings.cpp)

usage: python tools/find_sdk_functions.py --ref-elf <sotc_linked.elf> --ref-csv <sotc_functions.csv>
       (defaults come from URBAN_SDK_REF_ELF / URBAN_SDK_REF_CSV)
"""
import argparse
import collections
import csv
import difflib
import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SDK_NAME = re.compile(r"^(i?sce|_sce)")
TEXT_START, TEXT_END = 0x100000, 0x346D38
SIG_PREFIX = 6

# libkernel syscall numbers for the wrappers the runtime has API-level handlers for.
SYSCALL_WRAPPERS = {
    0x76: "sceSifDmaStat", -0x76: "isceSifDmaStat",
    0x77: "sceSifSetDma", -0x77: "isceSifSetDma",
    0x78: "sceSifSetDChain", -0x78: "isceSifSetDChain",
    0x79: "sceSifSetReg",
    0x7A: "sceSifGetReg",
}
# libcdvd command numbers (N = 0x80000592 "ncmd" server, S = 0x80000593 "scmd" server).
CD_NCMD = {5: "sceCdSeek", 6: "sceCdStandby", 7: "sceCdStop", 8: "sceCdPause"}
CD_SCMD = {1: "sceCdReadClock", 3: "sceCdGetDiskType", 4: "sceCdGetError", 5: "sceCdTrayReq", 0x16: "sceCdBreak"}
BRANCH_OPS = {0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}


def load_elf(path):
    data = open(path, "rb").read()
    phoff = struct.unpack("<I", data[0x1C:0x20])[0]
    phnum = struct.unpack("<H", data[0x2C:0x2E])[0]
    segments = []
    for i in range(phnum):
        p = struct.unpack("<8I", data[phoff + i * 32:phoff + i * 32 + 32])
        if p[0] == 1:
            segments.append((p[2], p[1], p[4]))

    def read(addr, size):
        for vaddr, offset, filesz in segments:
            if vaddr <= addr and addr + size <= vaddr + filesz:
                return data[offset + addr - vaddr:offset + addr - vaddr + size]
        return None

    return read


def normalize_word(w):
    op = w >> 26
    if op in (0x02, 0x03):
        return w & 0xFC000000          # j/jal target
    if op in (0x00, 0x10, 0x11, 0x12, 0x13, 0x1C) or op in BRANCH_OPS:
        return w                       # R-type, COPx, MMI, branches (relative offsets survive relinking)
    return w & 0xFFFF0000              # I-type: addresses and offsets change between builds


def normalize(blob):
    return tuple(normalize_word(w) for w in struct.unpack("<%dI" % (len(blob) // 4), blob))


def similarity(a, b):
    if abs(len(a) - len(b)) > max(4, len(a) // 5):
        return 0.0
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", default=os.path.join(ROOT, "build", "SLUS_212.09"))
    parser.add_argument("--ref-elf", default=os.environ.get("URBAN_SDK_REF_ELF"))
    parser.add_argument("--ref-csv", default=os.environ.get("URBAN_SDK_REF_CSV"))
    parser.add_argument("--functions-header", default=os.path.join(ROOT, "generated", "ps2_recompiled_functions.h"))
    parser.add_argument("--out", default=os.path.join(ROOT, "Port", "src", "urban", "sdk_functions.inc"))
    args = parser.parse_args()
    if not args.ref_elf or not args.ref_csv:
        sys.exit("pass --ref-elf/--ref-csv (e.g. the SotC port's build/link/sotc_linked.elf and sotc_functions.csv)")

    ref_read = load_elf(args.ref_elf)
    urban_read = load_elf(args.elf)
    text = urban_read(TEXT_START, TEXT_END - TEXT_START)
    words = [normalize_word(w) for w in struct.unpack("<%dI" % (len(text) // 4), text)]
    raw_words = struct.unpack("<%dI" % (len(text) // 4), text)

    def urban_sig(addr, length):
        i = (addr - TEXT_START) // 4
        return tuple(words[i:i + length])

    index = collections.defaultdict(list)
    for i in range(len(words) - SIG_PREFIX):
        index[tuple(words[i:i + SIG_PREFIX])].append(i)

    reference = []
    for row in csv.DictReader(open(args.ref_csv)):
        name = row["name"]
        if name.startswith("sub_"):
            continue
        start, end = int(row["start"], 16), int(row["end"], 16)
        blob = ref_read(start, end - start) if end - start >= 8 else None
        if blob:
            reference.append((name, start, normalize(blob)))
    reference.sort(key=lambda r: r[1])

    found = {}          # urban address -> (name, method, score)
    by_name = {}        # reference name -> urban address

    def accept(addr, name, method, score):
        if addr in found or name in by_name:
            return False
        found[addr] = (name, method, score)
        by_name[name] = addr
        return True

    # syscall wrappers first: they are unambiguous and too short for signature matching
    for i in range(len(raw_words) - 2):
        w = raw_words[i]
        if (w >> 16) == 0x2403 and raw_words[i + 1] == 0x0000000C and raw_words[i + 2] == 0x03E00008:
            number = w & 0xFFFF
            number = number - 0x10000 if number & 0x8000 else number
            if number in SYSCALL_WRAPPERS:
                accept(TEXT_START + i * 4, SYSCALL_WRAPPERS[number], "syscall", 1.0)

    # pass 1: exact, unique
    for name, _, sig in reference:
        if len(sig) < SIG_PREFIX:
            continue
        hits = [i for i in index.get(sig[:SIG_PREFIX], []) if tuple(words[i:i + len(sig)]) == sig]
        if len(hits) == 1:
            accept(TEXT_START + hits[0] * 4, name, "exact", 1.0)

    # pass 2: near predicted address, exact or fuzzy
    for idx, (name, start, sig) in enumerate(reference):
        if name in by_name or len(sig) < 2:
            continue
        guesses = []
        for step in (-1, 1):
            j = idx + step
            while 0 <= j < len(reference) and reference[j][0] not in by_name:
                j += step
            if 0 <= j < len(reference) and abs(reference[j][1] - start) < 0x4000:
                guesses.append(by_name[reference[j][0]] + (start - reference[j][1]))
        best = (0.0, None)
        for guess in guesses:
            for delta in range(-0x400, 0x404, 4):
                addr = guess + delta
                if not TEXT_START <= addr < TEXT_END:
                    continue
                candidate = urban_sig(addr, len(sig))
                if candidate[:2] != sig[:2]:
                    continue
                if candidate != sig and len(sig) < 8:
                    continue        # short functions only match exactly; fuzzy hits on them are noise
                score = 1.0 if candidate == sig else similarity(sig, candidate)
                if score > best[0]:
                    best = (score, addr)
        if best[0] >= 0.80:
            accept(best[1], name, "near", best[0])

    # pass 3: order between anchors (needs the recompiler's function starts)
    starts = sorted(int(m, 16) for m in re.findall(r"sub_([0-9A-F]{8})_0x", open(args.functions_header).read()))
    # Only SDK functions anchor this pass: a false-positive game match must not shift a whole run.
    matched = [(i, by_name[r[0]]) for i, r in enumerate(reference) if r[0] in by_name and SDK_NAME.match(r[0])]
    for (i0, a0), (i1, a1) in zip(matched, matched[1:]):
        gap = reference[i0 + 1:i1]
        if not gap or a1 <= a0:
            continue
        inner = [s for s in starts if a0 < s < a1]
        if len(inner) == len(gap) and all(reference[i0 + 1 + k][1] - reference[i0][1] < 0x4000 for k in range(len(gap))):
            for (name, _, _), addr in zip(gap, inner):
                accept(addr, name, "order", 0.0)

    # pass 4: libcdvd functions by RPC command
    if "sceSifCallRpc" in by_name and "sceCdRead" in by_name and "sceCdGetError" in by_name:
        call_rpc = by_name["sceSifCallRpc"]

        def rpc_calls(func, end):
            """(client, command) for each sceSifCallRpc call, tracking lui/addiu/ori constants into $a0/$a1."""
            regs, calls = {0: 0}, []
            for addr in range(func, end, 4):
                w = raw_words[(addr - TEXT_START) // 4]
                op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
                simm = imm - 0x10000 if imm & 0x8000 else imm
                if op == 0x0F:
                    regs[rt] = imm << 16
                elif op == 0x09 and rs in regs:
                    regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
                elif op == 0x0D and rs in regs:
                    regs[rt] = regs[rs] | imm
                elif op == 0x03:
                    slot = raw_words[(addr + 4 - TEXT_START) // 4]          # the delay slot executes first
                    if slot >> 26 == 0x09 and ((slot >> 21) & 31) in regs:
                        s_imm = slot & 0xFFFF
                        regs[(slot >> 16) & 31] = (regs[(slot >> 21) & 31] + (s_imm - 0x10000 if s_imm & 0x8000 else s_imm)) & 0xFFFFFFFF
                    if (w & 0x03FFFFFF) << 2 == call_rpc:
                        calls.append((regs.get(4), regs.get(5)))
            return calls

        bounds = sorted(set(starts) | set(found))
        next_start = {a: b for a, b in zip(bounds, bounds[1:])}
        ncmd = {c for c, _ in rpc_calls(by_name["sceCdRead"], next_start[by_name["sceCdRead"]])}
        scmd = {c for c, _ in rpc_calls(by_name["sceCdGetError"], next_start[by_name["sceCdGetError"]])}
        lo, hi = min(by_name["sceCdRead"], by_name["sceCdGetError"]) - 0x2000, max(by_name["sceCdRead"], by_name["sceCdGetError"]) + 0x2000
        for func in bounds:
            if not lo <= func < hi or func in found or func not in next_start:
                continue
            for client, command in rpc_calls(func, next_start[func]):
                name = (CD_NCMD if client in ncmd else CD_SCMD if client in scmd else {}).get(command)
                if name:
                    accept(func, name, "rpc", 1.0)
                    break

    rows = sorted((a, n, m, s) for a, (n, m, s) in found.items() if SDK_NAME.match(n))
    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write("// Generated by tools/find_sdk_functions.py - do not edit.\n")
        out.write("// X(guest address, SDK name, match method, similarity)\n")
        for addr, name, method, score in rows:
            out.write(f'X(0x{addr:08X}u, {name}, "{method}", {score:.2f}f)\n')
    counts = collections.Counter(m for _, _, m, _ in rows)
    print(f"{len(rows)} SDK functions -> {os.path.relpath(args.out, ROOT)} ({dict(counts)})")


if __name__ == "__main__":
    main()
