"""Minimal ISO9660 lister/extractor (no dependencies).

Usage:
  python tools/iso_extract.py <iso> [--out DIR] [--match SUFFIX ...]
Lists every file (path, LBA, size). With --out, files whose name ends with one of the --match suffixes
(case-insensitive, default: every file) are copied to DIR.
"""
import argparse
import os
import struct

SECTOR = 2048


def read_dir(f, lba, size, prefix, out):
    f.seek(lba * SECTOR)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:
            pos = (pos // SECTOR + 1) * SECTOR  # records never cross a sector
            continue
        rec = data[pos:pos + length]
        ext_lba = struct.unpack_from('<I', rec, 2)[0]
        ext_size = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len]
        pos += length
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('latin1').split(';')[0]
        path = prefix + '/' + name
        if flags & 2:
            read_dir(f, ext_lba, ext_size, path, out)
        else:
            out.append((path, ext_lba, ext_size))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('iso')
    ap.add_argument('--out')
    ap.add_argument('--match', action='append', default=[])
    a = ap.parse_args()
    with open(a.iso, 'rb') as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        root = pvd[156:156 + 34]
        files = []
        read_dir(f, struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '', files)
        for path, lba, size in files:
            print(f'{path}\t{lba}\t{size}')
            if a.out and (not a.match or any(path.lower().endswith(m.lower()) for m in a.match)):
                os.makedirs(a.out, exist_ok=True)
                f.seek(lba * SECTOR)
                with open(os.path.join(a.out, os.path.basename(path)), 'wb') as o:
                    o.write(f.read(size))


if __name__ == '__main__':
    main()
