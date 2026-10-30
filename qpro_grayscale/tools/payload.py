#!/usr/bin/env python3
"""Pull partitions out of a full A/B OTA zip. payload.bin is stored uncompressed in
the zip, so it's read in place. Only the op types full OTAs use are handled.

  payload.py <ota.zip> <outdir> [partition ...]"""
import bz2
import lzma
import os
import struct
import sys
import zipfile


def varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            return r, i


def fields(b):
    """Flat protobuf decode: list of (field, value), length-delimited values left as bytes."""
    i, out = 0, []
    while i < len(b):
        k, i = varint(b, i)
        f, t = k >> 3, k & 7
        if t == 0:
            v, i = varint(b, i)
        elif t == 2:
            n, i = varint(b, i)
            v = b[i:i + n]
            i += n
        elif t == 1:
            v = b[i:i + 8]
            i += 8
        elif t == 5:
            v = b[i:i + 4]
            i += 4
        else:
            raise ValueError(f"wire type {t}")
        out.append((f, v))
    return out


def main():
    ota, outdir, want = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    os.makedirs(outdir, exist_ok=True)
    info = zipfile.ZipFile(ota).getinfo("payload.bin")
    if info.compress_type:
        sys.exit("payload.bin is compressed in the zip, not handled")
    f = open(ota, "rb")
    f.seek(info.header_offset)
    h = f.read(30)
    base = info.header_offset + 30 + struct.unpack_from("<H", h, 26)[0] + struct.unpack_from("<H", h, 28)[0]
    f.seek(base)
    magic, ver, msize, ssize = struct.unpack(">4sQQI", f.read(24))
    if magic != b"CrAU" or ver != 2:
        sys.exit("not a v2 payload")
    man = f.read(msize)
    data_start = base + 24 + msize + ssize

    bs = 4096
    parts = []
    for fn, v in fields(man):
        if fn == 3:
            bs = v
        elif fn == 13:
            parts.append(v)

    for p in parts:
        pf = fields(p)
        name = next(v for k, v in pf if k == 1).decode()
        if name not in want:
            continue
        with open(os.path.join(outdir, name + ".img"), "wb") as o:
            for op in (v for k, v in pf if k == 8):
                d, ext = {}, []
                for k, v in fields(op):
                    if k == 6:
                        ext.append(dict(fields(v)))
                    else:
                        d[k] = v
                typ = d.get(1, 0)
                f.seek(data_start + d.get(2, 0))
                blob = f.read(d.get(3, 0))
                if typ == 0:
                    raw = blob
                elif typ == 1:
                    raw = bz2.decompress(blob)
                elif typ == 8:
                    raw = lzma.decompress(blob)
                elif typ == 6:
                    raw = None
                else:
                    sys.exit(f"{name}: op type {typ} not handled")
                pos = 0
                for e in ext:
                    n = e.get(2, 0) * bs
                    o.seek(e.get(1, 0) * bs)
                    o.write(b"\0" * n if raw is None else raw[pos:pos + n])
                    pos += n
        print("wrote", name)


if __name__ == "__main__":
    main()
