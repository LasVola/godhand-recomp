"""Decode a VIF1 or GIF DMA source chain from a guest RAM dump (URBAN_DUMP_RAM).

usage: python tools/vif_chain.py <ram.bin> <tag address hex> [--gif] [--max-tags N]
VIF1 (default): the tag's upper 64 bits (TTE) and the transferred data form one VIF code stream; DIRECT/DIRECTHL
payloads are decoded as GIF packets. --gif: the transferred data is GIF packets (PATH3).
"""
import argparse
import struct

TAG_IDS = {0: "refe", 1: "cnt", 2: "next", 3: "ref", 4: "refs", 5: "call", 6: "ret", 7: "end"}
VIF_NAMES = {0x00: "NOP", 0x01: "STCYCL", 0x02: "OFFSET", 0x03: "BASE", 0x04: "ITOP", 0x05: "STMOD", 0x06: "MSKPATH3",
             0x07: "MARK", 0x10: "FLUSHE", 0x11: "FLUSH", 0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT",
             0x20: "STMASK", 0x30: "STROW", 0x31: "STCOL", 0x4A: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}


GS_REGS = {0x00: "PRIM", 0x01: "RGBAQ", 0x02: "ST", 0x03: "UV", 0x04: "XYZF2", 0x05: "XYZ2", 0x06: "TEX0_1", 0x07: "TEX0_2",
           0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x0A: "FOG", 0x0D: "XYZF3", 0x0E: "XYZ3", 0x14: "TEX1_1", 0x16: "TEX2_1",
           0x18: "XYOFFSET_1", 0x1A: "PRMODECONT", 0x1B: "PRMODE", 0x1C: "TEXCLUT", 0x22: "SCANMSK", 0x3B: "TEXA",
           0x3D: "FOGCOL", 0x3F: "TEXFLUSH", 0x40: "SCISSOR_1", 0x42: "ALPHA_1", 0x44: "DIMX", 0x45: "DTHE",
           0x46: "COLCLAMP", 0x47: "TEST_1", 0x49: "PABE", 0x4A: "FBA_1", 0x4C: "FRAME_1", 0x4E: "ZBUF_1",
           0x50: "BITBLTBUF", 0x51: "TRXPOS", 0x52: "TRXREG", 0x53: "TRXDIR", 0x54: "HWREG", 0x60: "SIGNAL",
           0x61: "FINISH", 0x62: "LABEL"}


def gif_packets(data):
    """Summarize GIF packets: one line per tag, A+D register writes listed."""
    i = 0
    out = []
    while i + 16 <= len(data):
        lo, regs = struct.unpack_from("<QQ", data, i)
        nloop = lo & 0x7FFF
        eop = (lo >> 15) & 1
        pre = (lo >> 46) & 1
        prim = (lo >> 47) & 0x7FF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        i += 16
        if flg == 0:   # PACKED
            names = []
            ad = []
            for n in range(nreg):
                r = (regs >> (4 * n)) & 0xF
                names.append({0: "PRIM", 1: "RGBAQ", 2: "ST", 3: "UV", 4: "XYZF2", 5: "XYZ2", 0xE: "A+D", 0xF: "NOP"}.get(r, f"r{r:x}"))
            for loop in range(nloop):
                for n in range(nreg):
                    if (regs >> (4 * n)) & 0xF == 0xE and i + 16 <= len(data):
                        value, addr = struct.unpack_from("<QQ", data, i)
                        ad.append(f"{GS_REGS.get(addr & 0xFF, hex(addr & 0xFF))}=0x{value:x}")
                    i += 16
            desc = f"PACKED nloop={nloop} regs={','.join(names)}" + (f" prim=0x{prim:x}" if pre else "")
            out.append(desc + (" eop" if eop else ""))
            for a in ad[:12]:
                out.append("        " + a)
            if len(ad) > 12:
                out.append(f"        ... {len(ad) - 12} more A+D")
        elif flg == 1:  # REGLIST
            i += (nloop * nreg * 8 + 15) // 16 * 16
            out.append(f"REGLIST nloop={nloop} nreg={nreg}" + (" eop" if eop else ""))
        else:           # IMAGE
            i += nloop * 16
            out.append(f"IMAGE {nloop} qw" + (" eop" if eop else ""))
    return out


def vif_codes(data, wl=1, cl=1):
    """Yield (offset, text) for VIF codes in a byte string."""
    i = 0
    while i + 4 <= len(data):
        code = struct.unpack_from("<I", data, i)[0]
        cmd = (code >> 24) & 0x7F
        imm = code & 0xFFFF
        num = (code >> 16) & 0xFF
        irq = " IRQ" if code & 0x80000000 else ""
        i += 4
        if cmd in (0x01,):
            wl, cl = (imm >> 8) & 0xFF, imm & 0xFF
            yield i - 4, f"STCYCL cl={cl} wl={wl}{irq}"
        elif cmd == 0x20:
            yield i - 4, f"STMASK{irq}"
            i += 4
        elif cmd in (0x30, 0x31):
            yield i - 4, f"{VIF_NAMES[cmd]}{irq}"
            i += 16
        elif cmd == 0x4A:
            n = num or 256
            yield i - 4, f"MPG addr=0x{imm * 8:x} size={n * 8}{irq}"
            i += n * 8
        elif cmd in (0x50, 0x51):
            n = imm or 65536
            yield i - 4, f"{VIF_NAMES[cmd]} {n} qw{irq}"
            i = (i + 15) // 16 * 16 if i % 16 else i   # DIRECT data starts on a quadword boundary
            for line in gif_packets(data[i:i + n * 16]):
                yield i, "    gif: " + line
            i += n * 16
        elif cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            bits = (32 >> vl) * (vn + 1)
            if wl <= cl:
                length = n
            else:
                length = cl * (n // wl) + min(n % wl, cl)
            size = (length * bits + 31) // 32 * 4
            yield i - 4, f"UNPACK V{vn + 1}-{32 >> vl} addr=0x{imm & 0x3FF:x}{' +TOPS' if imm & 0x8000 else ''} num={n}{irq}"
            i += size
        else:
            name = VIF_NAMES.get(cmd, f"?{cmd:02x}")
            extra = f" 0x{imm:x}" if cmd in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15) else ""
            yield i - 4, f"{name}{extra}{irq}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ram")
    ap.add_argument("tag", type=lambda v: int(v, 16))
    ap.add_argument("--gif", action="store_true", help="GIF channel chain (PATH3) instead of VIF1")
    ap.add_argument("--max-tags", type=int, default=64)
    args = ap.parse_args()
    ram = open(args.ram, "rb").read()
    addr = args.tag
    stack = []
    stream = bytearray()   # VIF1: tag upper words + data form one code stream
    for _ in range(args.max_tags):
        lo, hi = struct.unpack_from("<QQ", ram, addr & 0x1FFFFFF)
        qwc = lo & 0xFFFF
        tid = (lo >> 28) & 7
        taddr = (lo >> 32) & 0x7FFFFFFF
        irq = " IRQ" if lo & 0x80000000 else ""
        print(f"tag @0x{addr:08x} {TAG_IDS[tid]:4} qwc={qwc} addr=0x{taddr:08x}{irq}")
        data_addr = addr + 16 if tid in (1, 2, 5, 6, 7) else taddr
        data = ram[data_addr & 0x1FFFFFF:(data_addr & 0x1FFFFFF) + qwc * 16]
        if args.gif:
            for line in gif_packets(data):
                print("      " + line)
        else:
            # Tag words occupy the second half of the tag quadword; pad so DIRECT data aligns to the next quadword.
            stream += bytes(8) + struct.pack("<Q", hi) + data
        if tid in (0, 7):
            break
        if tid == 1:
            addr = data_addr + qwc * 16
        elif tid == 2:
            addr = taddr
        elif tid in (3, 4):
            addr = addr + 16
        elif tid == 5:
            stack.append(data_addr + qwc * 16)
            addr = taddr
        elif tid == 6:
            if not stack:
                break
            addr = stack.pop()
    if not args.gif:
        print("VIF stream:")
        for off, text in vif_codes(bytes(stream)):
            print(f"  +{off:05x} {text}")


if __name__ == "__main__":
    main()
