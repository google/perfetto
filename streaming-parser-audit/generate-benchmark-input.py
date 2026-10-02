#!/usr/bin/env python3
"""Generate monotonically ordered ftrace-only scheduling workloads."""
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
for stalled in (False, True):
  name = 'sched-pinned.pftrace' if stalled else 'sched-moving.pftrace'
  with (root / name).open('wb') as output:
    for i in range(count):
      cpu = 4 if stalled and i == 0 else i % 4
      prev_pid = 0 if i < 4 or (stalled and i == 4) else cpu + 1
      sched = (
          message(1, b'task') + integer(2, prev_pid) + integer(3, 120) +
          integer(4, 0) + message(5, b'task') + integer(6, cpu + 1) +
          integer(7, 120))
      event = integer(1, 100 + i * 100) + integer(2, prev_pid) + message(
          4, sched)
      bundle = integer(1, cpu) + message(2, event)
      output.write(message(1, message(1, bundle)))
      if (i + 1) % 8192 == 0:
        output.write(sorter_checkpoint(100 + i * 100))
  print(name, (root / name).stat().st_size)
