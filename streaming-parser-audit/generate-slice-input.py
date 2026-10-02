#!/usr/bin/env python3
"""Ordered TrackEvent slices under a long-lived parent, with real args."""
from pathlib import Path
import sys


def varint(value):
  out = bytearray()
  while value > 127:
    out.append((value & 127) | 128)
    value >>= 7
  out.append(value)
  return bytes(out)


def integer(field, value):
  return varint(field * 8) + varint(value)


def message(field, value):
  return varint(field * 8 + 2) + varint(len(value)) + value


def packet(ts, event):
  return message(1, integer(8, ts) + integer(10, 1) + message(11, event))


def annotation(name, field, value):
  return message(
      4,
      message(10, name) + (message(field, value) if isinstance(value, bytes)
                           else integer(field, value)))


def sorter_checkpoint(ts):
  # Standard tracing-service protocol: two producer-flush completion notifications and a
  # read-completed notification. The sorter keeps its normal one-cycle lag.
  return b''.join(
      message(1,
              integer(8, ts) + message(69, integer(field, 1)))
      for field in (3, 3, 4))


count = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
root = Path(sys.argv[2]) if len(
    sys.argv) > 2 else Path(__file__).resolve().parent
root.mkdir(parents=True, exist_ok=True)
workloads = (False, True) if len(sys.argv) < 4 else (sys.argv[3] == 'unique',)
for unique in workloads:
  path = root / ('slices-unique.pftrace'
                 if unique else 'slices-repeated.pftrace')
  with path.open('wb') as output:
    descriptor = integer(1, 123) + message(2, b'slice stream')
    output.write(
        message(1,
                integer(8, 1) + integer(10, 1) + message(60, descriptor)))
    output.write(
        packet(
            2,
            integer(9, 1) + integer(11, 123) + message(23, b'outer') +
            annotation(b'root', 4, 1)))
    for i in range(count):
      begin = (
          integer(9, 1) + integer(11, 123) + message(23, b'child') +
          message(22, b'benchmark') +
          annotation(b'index', 4, i if unique else i % 64) +
          annotation(b'label', 6, b'constant'))
      end = integer(9, 2) + integer(11, 123) + annotation(b'finished', 2, 1)
      output.write(packet(10 + i * 10, begin))
      output.write(packet(15 + i * 10, end))
      if (i + 1) % 8192 == 0:
        output.write(sorter_checkpoint(15 + i * 10))
    output.write(packet(count * 10 + 20, integer(9, 2) + integer(11, 123)))
  print(path, path.stat().st_size)
