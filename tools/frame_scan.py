"""Find generated functions that MSVC compiled without optimization, by their stack frames.

Unoptimized code keeps every temporary on the stack, so a recompiled function gets a frame of tens or hundreds of KB;
any frame above 4 KB calls __chkstk with the size in eax (mov eax, imm32; call __chkstk). This reads the COFF objects
of the generated code, finds each call to __chkstk and reports the function and frame size.

usage: python tools/frame_scan.py [build/port/src/CMakeFiles] [--min 16384]
"""
import argparse
import struct
import sys
from pathlib import Path


def scan(path: Path):
    data = path.read_bytes()
    if len(data) < 20:
        return []
    machine, nsec, _, symptr, nsym, optsize, _ = struct.unpack_from("<HHIIIHH", data, 0)
    if machine == 0 and nsec == 0xFFFF:  # bigobj: ANON_OBJECT_HEADER_BIGOBJ
        nsec, symptr, nsym = struct.unpack_from("<III", data, 44)
        sec_off = 56
        sym_size = 20
    else:
        sec_off = 20 + optsize
        sym_size = 18
    strtab = symptr + nsym * sym_size
    sections = []
    for i in range(nsec):
        name, vsize, vaddr, rawsize, rawptr, relptr, _, nrel, _, chars = struct.unpack_from("<8sIIIIIIHHI", data, sec_off + i * 40)
        sections.append((name, rawsize, rawptr, relptr, nrel))

    def sym_name(index):
        off = symptr + index * sym_size
        raw = data[off:off + 8]
        if raw[:4] == b"\0\0\0\0":
            soff = struct.unpack_from("<I", raw, 4)[0]
            end = data.index(b"\0", strtab + soff)
            return data[strtab + soff:end].decode("latin-1")
        return raw.rstrip(b"\0").decode("latin-1")

    # Function symbols by section (1-based section numbers), plus the __chkstk symbol index.
    funcs = {}
    chkstk = set()
    i = 0
    while i < nsym:
        off = symptr + i * sym_size
        value = struct.unpack_from("<I", data, off + 8)[0]
        if sym_size == 20:
            secnum = struct.unpack_from("<i", data, off + 12)[0]
            typ, sclass, naux = struct.unpack_from("<HBB", data, off + 16)
        else:
            secnum = struct.unpack_from("<h", data, off + 12)[0]
            typ, sclass, naux = struct.unpack_from("<HBB", data, off + 14)
        name = sym_name(i)
        if name == "__chkstk":
            chkstk.add(i)
        if typ == 0x20 and secnum > 0:  # function
            funcs.setdefault(secnum, []).append((value, name))
        i += 1 + naux
    if not chkstk:
        return []
    found = []
    for secnum, (name, rawsize, rawptr, relptr, nrel) in enumerate(sections, start=1):
        for r in range(nrel):
            vaddr, symidx, rtype = struct.unpack_from("<IIH", data, relptr + r * 10)
            if symidx not in chkstk:
                continue
            # mov eax, imm32 (B8 xx xx xx xx) right before the call's E8 opcode
            call_at = rawptr + vaddr - 1
            size = None
            if data[call_at - 5] == 0xB8:
                size = struct.unpack_from("<I", data, call_at - 4)[0]
            owner = "?"
            for value, fname in sorted(funcs.get(secnum, [])):
                if value <= vaddr:
                    owner = fname
            found.append((size or 0, owner, path.name))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default="build/port/src/CMakeFiles")
    ap.add_argument("--min", type=int, default=16384, help="report frames at least this big")
    a = ap.parse_args()
    results = []
    objs = [p for p in Path(a.root).rglob("*.obj") if "generated" in str(p)]
    for p in objs:
        try:
            results.extend(scan(p))
        except Exception as e:  # noqa: BLE001
            print(f"{p}: {e}", file=sys.stderr)
    results.sort(reverse=True)
    print(f"{len(objs)} objects, {len(results)} calls to __chkstk")
    for size, owner, obj in results:
        if size >= a.min:
            print(f"{size:>9}  {owner}  ({obj})")


if __name__ == "__main__":
    main()
