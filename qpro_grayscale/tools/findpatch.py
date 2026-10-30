#!/usr/bin/env python3
"""Locate the grayscale patch site in a libmrengine.so and print a BUILDS line for common.sh.

The site: the selector that gives only device type 2 (Quest 2) the mono-only FrameSetConsumer
table. It sits just before the "does not have any FrameSetConsumers ... Defaulting to grayscale
only" log call:  ldrb w4,[x19,#0x60] ; cmp w4,#2 ; b.ne <other devices>.  The b.ne becomes a nop.

  findpatch.py <libmrengine.so> <build incremental>"""
import hashlib
import struct
import sys

NOP = 0xd503201f


def sections(d):
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3a)
    sh = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = sh[shstrndx][4]
    name = lambda off: d[strtab + off:d.index(b"\0", strtab + off)].decode()
    return {name(s[0]): (s[3], s[4], s[5]) for s in sh}  # addr, offset, size


def main():
    path, build = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    secs = sections(d)
    taddr, toff, tsz = secs[".text"]
    s = d.find(b"does not have any FrameSetConsumers")
    while d[s - 1]:
        s -= 1

    # adrp+add pairs resolving to the log string
    refs, regs = [], {}
    for i in range(0, tsz, 4):
        w, = struct.unpack_from("<I", d, toff + i)
        pc = taddr + i
        if (w & 0x9f000000) == 0x90000000:
            imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            regs[w & 31] = ((pc & ~0xfff) + (imm << 12), pc)
        elif (w & 0xff800000) == 0x91000000:
            rn = (w >> 5) & 31
            imm = (w >> 10) & 0xfff
            if rn in regs and pc - regs[rn][1] < 64 and regs[rn][0] + imm == s:
                refs.append(pc)
    if len(refs) != 1:
        sys.exit(f"expected one reference to the grayscale log, found {len(refs)}")

    # walk back for ldrb w4,[x19,#0x60] ; cmp w4,#2 ; b.ne
    want_ldrb = 0x39418264
    want_cmp = 0x7100089f
    hits = []
    for pc in range(refs[0] - 4, refs[0] - 0x1000, -4):
        o = toff + (pc - taddr)
        a, b, c = struct.unpack_from("<III", d, o)
        if a == want_ldrb and b == want_cmp and (c & 0xff00001f) == 0x54000001:
            hits.append(o + 8)
    if len(hits) != 1:
        sys.exit(f"expected one selector, found {len(hits)}")
    off = hits[0]

    stock = hashlib.sha256(d).hexdigest()
    p = bytearray(d)
    struct.pack_into("<I", p, off, NOP)
    patched = hashlib.sha256(p).hexdigest()
    print(f"{stock} {off:#x} {patched} {build}")


if __name__ == "__main__":
    main()
