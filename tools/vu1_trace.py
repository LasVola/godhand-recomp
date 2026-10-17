"""Reads a VU1 trace (PS2X_VU1_TRACE or the F8 capture: vu1trace_<tick>.bin).

Record: magic 'V1TR' (0x52543156), maxCycles u32, image hash u64, tick u64, TraceState (starts with VU1State:
vf[32][4] f32, vi[16] i32, acc[4], q, p, i, r, pc, ...), then data-memory deltas against the previous record:
(u16 qword index, u16 qword count, data...) until (0xFFFF, 0).

Usage:
  python tools/vu1_trace.py trace.bin                      # summary per start PC
  python tools/vu1_trace.py trace.bin --record N --dump A:L  # VU data qwords A..A+L (in qwords) as floats
"""
import argparse
import struct
from collections import Counter

MAGIC = 0x52543156
STATE_PC_OFFSET = 512 + 64 + 16 + 16


def find_state_size(blob):
    for size in range(600, 8000, 8):
        pos = 24 + size
        ok = True
        while True:
            if pos + 4 > len(blob):
                ok = False
                break
            idx, cnt = struct.unpack_from("<HH", blob, pos)
            pos += 4
            if idx == 0xFFFF:
                break
            if idx >= 1024 or cnt == 0 or idx + cnt > 1024:
                ok = False
                break
            pos += cnt * 16
        if ok and pos + 4 <= len(blob) and struct.unpack_from("<I", blob, pos)[0] == MAGIC:
            return size
    raise SystemExit("could not find TraceState size")


def records(blob):
    size = find_state_size(blob)
    mem = bytearray(0x4000)
    pos = 0
    n = 0
    while pos + 24 <= len(blob):
        magic, maxc, h, tick = struct.unpack_from("<IIQQ", blob, pos)
        if magic != MAGIC:
            break
        state = blob[pos + 24:pos + 24 + size]
        pos += 24 + size
        while True:
            idx, cnt = struct.unpack_from("<HH", blob, pos)
            pos += 4
            if idx == 0xFFFF:
                break
            mem[idx * 16:(idx + cnt) * 16] = blob[pos:pos + cnt * 16]
            pos += cnt * 16
        yield n, tick, h, state, bytes(mem)
        n += 1


def vf(state, r):
    return struct.unpack_from("<4f", state, r * 16)


def vi(state, r):
    return struct.unpack_from("<i", state, 512 + r * 4)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--record", type=int)
    ap.add_argument("--dump")
    ap.add_argument("--regs", action="store_true")
    a = ap.parse_args()
    blob = open(a.trace, "rb").read()
    if a.record is None:
        pcs = Counter()
        first = {}
        for n, tick, h, state, mem in records(blob):
            pc = struct.unpack_from("<I", state, STATE_PC_OFFSET)[0]
            pcs[(tick, pc)] += 1
            first.setdefault((tick, pc), n)
        for (tick, pc), c in sorted(pcs.items()):
            print(f"tick {tick} pc 0x{pc:04x}: {c} runs (first record {first[(tick, pc)]})")
        return
    for n, tick, h, state, mem in records(blob):
        if n != a.record:
            continue
        pc = struct.unpack_from("<I", state, STATE_PC_OFFSET)[0]
        print(f"record {n} tick {tick} pc 0x{pc:04x}")
        if a.regs:
            for r in range(32):
                print(f"  vf{r:<2} " + " ".join(f"{x:12.5g}" for x in vf(state, r)))
            print("  vi " + " ".join(str(vi(state, r)) for r in range(16)))
        if a.dump:
            start, length = (int(x, 0) for x in a.dump.split(":"))
            for q in range(start, start + length):
                f = struct.unpack_from("<4f", mem, q * 16)
                w = struct.unpack_from("<4I", mem, q * 16)
                print(f"  {q:4d} " + " ".join(f"{x:12.5g}" for x in f) + "   " + " ".join(f"{x:08x}" for x in w))
        break


if __name__ == "__main__":
    main()
