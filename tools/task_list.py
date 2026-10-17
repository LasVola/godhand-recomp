"""Walks Urban Reign's task list (the objects FUN_001236c8 updates and draws every frame) in a RAM image.

usage: python tools/task_list.py <ram.bin | pine> [--root 0xac0a80]

Each task: next pointer at +0x00, parent link at +0x04, kind/flags word at +0x0C (low 5 bits index the draw table
at 0x34baf8), state at +0x24, method table at +0x4C. One line per task; with --pine the RAM is read from a running
PCSX2 through PINE (tools/pine.py), read-only.
"""
import argparse
import struct
import sys
from pathlib import Path


class Ram:
    def __init__(self, data=None, pine=None):
        self.data = data
        self.pine = pine

    def u32(self, addr):
        addr &= 0x1FFFFFF
        if self.data is not None:
            return struct.unpack_from("<I", self.data, addr)[0]
        return self.pine.read32(addr)

    def u16(self, addr):
        return self.u32(addr & ~3) >> ((addr & 2) * 8) & 0xFFFF


def walk(ram, root, limit=4000):
    node = ram.u32(root + 0x10)
    seen = set()
    while node and node not in seen and len(seen) < limit:
        seen.add(node)
        flags = ram.u32(node + 0x0C)
        yield node, flags, ram.u16(node + 0x24), ram.u32(node + 0x4C), ram.u32(node + 0x04)
        node = ram.u32(node)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ram", help="RAM image (32 MB) or pine")
    ap.add_argument("--root", default="0xac0a80")
    a = ap.parse_args()
    if a.ram == "pine":
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import pine
        ram = Ram(pine=pine.Pine())
    else:
        ram = Ram(data=open(a.ram, "rb").read())
    root = int(a.root, 16)
    for node, flags, state, methods, parent in walk(ram, root):
        draw = ram.u32(0x34BAF8 + (flags & 0x1F) * 4)
        print(f"{node:08x} kind={flags & 0xFFFF:04x} flags={flags >> 16:04x} state={state} methods={methods:08x}"
              f" parent={parent:08x} draw={draw:08x}")


if __name__ == "__main__":
    main()
