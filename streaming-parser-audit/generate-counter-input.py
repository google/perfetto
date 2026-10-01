#!/usr/bin/env python3
"""Ordered counters, with normal tracing-service sorter checkpoints."""
from pathlib import Path
import struct
import sys

def varint(v):
    out = bytearray()
    while v > 127:
        out.append((v & 127) | 128)
        v >>= 7
    out.append(v)
    return bytes(out)

def integer(f, v):
    return varint(f * 8) + varint(v)

def message(f, v):
    return varint(f * 8 + 2) + varint(len(v)) + v

count = int(sys.argv[1])
path = Path(sys.argv[2])
path.parent.mkdir(parents=True, exist_ok=True)
with path.open('wb') as output:
    descriptor = integer(1, 123) + message(2, b'counter stream') + message(8, b'')
    output.write(message(1, integer(8, 1) + integer(10, 1) + message(60, descriptor)))
    for i in range(count):
        ts = 10 + i
        event = integer(9, 4) + integer(11, 123) + varint(44 * 8 + 1) + struct.pack('<d', i + .25)
        output.write(message(1, integer(8, ts) + integer(10, 1) + message(11, event)))
        if (i + 1) % 8192 == 0:
            for field in (3, 3, 4):
                output.write(message(1, integer(8, ts) + message(69, integer(field, 1))))
print(path, path.stat().st_size)
