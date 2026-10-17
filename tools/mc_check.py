"""Check a PS2 memory card image (Mcd001.ps2, 528-byte pages with ECC): superblock, FAT chains, directory tree and
page ECC. usage: python tools/mc_check.py <card.ps2> [--dump-dir]"""
import struct
import sys

PAGE, SPARE = 512, 16
RAW = PAGE + SPARE


def ecc_calc(chunk):
    # PS2 memory card ECC over 128 bytes (same as PCSX2 / mymc).
    parity = [0] * 256
    cpmasks = [0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0]
    col = [0] * 256
    for i in range(256):
        c = 0
        for b in range(8):
            c ^= (i >> b) & 1
        parity[i] = c
    for i in range(256):
        v = 0
        for j, m in enumerate(cpmasks):
            v |= parity[i & m] << j
        col[i] = v
    column = 0
    lp0 = lp1 = 0
    for i, b in enumerate(chunk):
        column ^= col[b]
        if parity[b]:
            lp0 ^= ~i & 0x7F
            lp1 ^= i
    return bytes([(~column) & 0x77, (~lp0) & 0x7F, (~lp1) & 0x7F])


class Card:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        sb = self.page(0)
        assert sb[:28] == b"Sony PS2 Memory Card Format ", sb[:28]
        (self.page_len, self.ppc, self.ppb, _, self.clusters, self.alloc_off, self.alloc_end, self.root,
         self.bb1, self.bb2) = struct.unpack_from("<HHHHIIIIII", sb, 0x28)
        self.ifc = struct.unpack_from("<32I", sb, 0x50)
        self.card_type, self.flags = sb[0x150 + 128], sb[0x150 + 129]
        self.cluster_size = self.page_len * self.ppc

    def page(self, n):
        return self.d[n * RAW:n * RAW + PAGE]

    def spare(self, n):
        return self.d[n * RAW + PAGE:n * RAW + RAW]

    def cluster(self, n):
        return b"".join(self.page(n * self.ppc + i) for i in range(self.ppc))

    def fat_entry(self, n):
        per = self.cluster_size // 4
        ind = self.ifc[n // (per * per)]
        fat_cluster = struct.unpack_from("<I", self.cluster(ind), 4 * ((n // per) % per))[0]
        return struct.unpack_from("<I", self.cluster(fat_cluster), 4 * (n % per))[0]

    def chain(self, start):
        out, n, seen = [], start, set()
        while True:
            if n in seen:
                return out, "loop"
            seen.add(n)
            out.append(n)
            e = self.fat_entry(n)
            if e == 0xFFFFFFFF:
                return out, None  # allocated, last cluster
            if not e & 0x80000000:
                return out, "free cluster in chain"
            nxt = e & 0x7FFFFFFF
            if nxt >= self.alloc_end:
                return out, f"next {nxt} out of range"
            n = nxt

    def read_file(self, start, length):
        clusters, err = self.chain(start)
        data = b"".join(self.cluster(self.alloc_off + c) for c in clusters)
        return data[:length], clusters, err


def walk(card, cluster, length, path, owner, problems, dump):
    data, clusters, err = card.read_file(cluster, length * 512)
    if err:
        problems.append(f"{path}: directory chain {err} ({clusters[:8]})")
    for c in clusters:
        owner.setdefault(c, []).append(path + "/")
    for i in range(length):
        e = data[i * 512:(i + 1) * 512]
        if len(e) < 512:
            problems.append(f"{path}: entry {i} past chain end")
            break
        mode, _, size = struct.unpack_from("<HHI", e, 0)
        created = e[8:16]
        start, = struct.unpack_from("<I", e, 0x10)
        modified = e[0x18:0x20]
        name = e[0x40:0x60].split(b"\0")[0].decode("ascii", "replace")
        if not mode & 0x8000:
            continue
        if name in (".", ".."):
            continue
        sub = f"{path}/{name}"
        mtime = f"{modified[6] | modified[7] << 8:04d}-{modified[5]:02d}-{modified[4]:02d} {modified[3]:02d}:{modified[2]:02d}:{modified[1]:02d}"
        if mode & 0x20:
            if dump:
                print(f"DIR  {sub} entries={size} cluster={start} mtime={mtime}")
            walk(card, start, size, sub, owner, problems, dump)
        else:
            fdata, fcl, ferr = card.read_file(start, size) if size else (b"", [], None)
            if dump:
                print(f"FILE {sub} size={size} cluster={start} clusters={len(fcl)} mtime={mtime}")
            if ferr:
                problems.append(f"{sub}: chain {ferr}")
            elif size and len(fcl) * card.cluster_size < size:
                problems.append(f"{sub}: chain has {len(fcl)} clusters for {size} bytes")
            for c in fcl:
                owner.setdefault(c, []).append(sub)


def main():
    card = Card(sys.argv[1])
    dump = "--dump-dir" in sys.argv
    print(f"pages/cluster={card.ppc} clusters={card.clusters} alloc_offset={card.alloc_off} alloc_end={card.alloc_end} "
          f"root={card.root} flags=0x{card.flags:02x} ifc={card.ifc[:2]}")
    # ECC over every page that is not erased
    bad_ecc = []
    if card.flags & 0x01:
        for p in range(len(card.d) // RAW):
            pg, sp = card.page(p), card.spare(p)
            if pg == b"\xff" * PAGE:
                continue
            want = b"".join(ecc_calc(pg[i * 128:(i + 1) * 128]) for i in range(4))
            if sp[:12] != want:
                bad_ecc.append(p)
    print(f"pages with wrong ECC: {len(bad_ecc)} {bad_ecc[:20]}")
    problems, owner = [], {}
    root_entry = card.cluster(card.alloc_off + card.root)[:512]
    root_len, = struct.unpack_from("<I", root_entry, 4)
    walk(card, card.root, root_len, "", owner, problems, dump)
    shared = {c: o for c, o in owner.items() if len(o) > 1}
    for c, o in list(shared.items())[:20]:
        problems.append(f"cluster {c} used by {o}")
    # allocated but unreferenced
    leaked = [c for c in range(card.alloc_end - card.alloc_off) if card.fat_entry(c) & 0x80000000 and c not in owner]
    print(f"problems: {len(problems)}")
    for p in problems[:40]:
        print("  " + p)
    print(f"allocated clusters not referenced by any file: {len(leaked)} {leaked[:20]}")


if __name__ == "__main__":
    main()
