#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes samples/demo-trace.ab1: a simulated Sanger read of pOC-demo.

The file follows the public ABIF layout (big-endian directory of tagged
entries) with the tags Restricto reads: PBAS, PCON, PLOC, DATA 9-12, FWO_
and SMPL. Peaks are Gaussians; quality is low at both ends like a real read.
One base differs from the plasmid so alignment tools have something to find.

Usage: scripts/make_demo_trace.py [samples/pOC-demo.gb] [samples/demo-trace.ab1]
"""
import math
import random
import struct
import sys


def read_genbank_bases(path):
    bases, origin = [], False
    for line in open(path):
        if line.startswith("ORIGIN"):
            origin = True
        elif line.startswith("//"):
            break
        elif origin:
            bases.extend(c for c in line.upper() if c in "ACGTN")
    return "".join(bases)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "samples/pOC-demo.gb"
    dst = sys.argv[2] if len(sys.argv) > 2 else "samples/demo-trace.ab1"
    plasmid = read_genbank_bases(src)
    rng = random.Random(42)
    start, length = 950, 720
    read = list(plasmid[start:start + length])
    read[300] = "A" if read[300] != "A" else "G"   # a point mutation at plasmid base 1251
    read = "".join(read)

    spacing = 12
    scans = spacing * (length + 2)
    peaks = [spacing * (i + 1) + rng.randint(-1, 1) for i in range(length)]
    quality = []
    for i in range(length):
        edge = min(i, length - 1 - i)
        q = 8 + min(edge, 40) + rng.randint(-3, 3)
        quality.append(max(2, min(q, 60)))
    order = "GATC"
    channels = {b: [0.0] * scans for b in order}
    for i, b in enumerate(read):
        height = 300 + 20 * quality[i] + rng.randint(0, 200)
        for x in range(max(0, peaks[i] - 15), min(scans, peaks[i] + 16)):
            channels[b][x] += height * math.exp(-((x - peaks[i]) ** 2) / 8.0)
    for b in order:   # baseline noise
        channels[b] = [int(v + rng.randint(0, 30)) for v in channels[b]]

    entries = []   # (name, number, type, elsize, count, data)

    def add(name, number, etype, elsize, data, count=None):
        entries.append((name, number, etype, elsize, count if count is not None else len(data) // elsize, data))

    add(b"PBAS", 1, 2, 1, read.encode())
    add(b"PBAS", 2, 2, 1, read.encode())
    add(b"PCON", 1, 2, 1, bytes(quality))
    add(b"PCON", 2, 2, 1, bytes(quality))
    add(b"PLOC", 1, 4, 2, struct.pack(">%dh" % length, *peaks))
    add(b"PLOC", 2, 4, 2, struct.pack(">%dh" % length, *peaks))
    for k, b in enumerate(order):
        add(b"DATA", 9 + k, 4, 2, struct.pack(">%dh" % scans, *channels[b]))
    add(b"FWO_", 1, 2, 1, order.encode())
    name = b"pOC-demo_fwd_read"
    add(b"SMPL", 1, 18, 1, bytes([len(name)]) + name)

    header_size = 128
    blob = bytearray()
    dir_entries = []
    for (tag, number, etype, elsize, count, data) in entries:
        if len(data) <= 4:
            offset_field = data.ljust(4, b"\0")
        else:
            offset_field = struct.pack(">I", header_size + len(blob))
            blob += data
        dir_entries.append(struct.pack(">4sIHHII", tag, number, etype, elsize, count, len(data)) + offset_field
                           + struct.pack(">I", 0))
    dir_offset = header_size + len(blob)
    directory = b"".join(dir_entries)
    header = b"ABIF" + struct.pack(">H", 101)
    header += struct.pack(">4sIHHII", b"tdir", 1, 1023, 28, len(entries), len(directory))
    header += struct.pack(">I", dir_offset) + struct.pack(">I", 0)
    header = header.ljust(header_size, b"\0")
    with open(dst, "wb") as f:
        f.write(header + bytes(blob) + directory)
    print("wrote", dst, len(read), "bases")


if __name__ == "__main__":
    main()
