#!/usr/bin/env python3
# Copyright 2026 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Emits a Fuchsia trace holding "power" / "cpu_frequency" counter records, both
# well formed ones and ones violating the trace contract.

import math
import struct
import sys

# String indices interned below.
POWER = 1
CPU_FREQUENCY = 2
CPU = 3
VALUE = 4
CLUSTER = 5
NOT_A_NUMBER = 6


def push_word(w):
  sys.stdout.buffer.write(struct.pack('<Q', w))


def push_string(index, s):
  s_bytes = s.encode('utf-8')
  s_len = len(s_bytes)
  num_words = 1 + (s_len + 7) // 8
  header = (s_len << 32) | (index << 16) | (num_words << 4) | 2
  push_word(header)
  padded = s_bytes.ljust((num_words - 1) * 8, b'\x00')
  for i in range(0, len(padded), 8):
    push_word(struct.unpack('<Q', padded[i:i + 8])[0])


# Argument builders. Each returns the words encoding one record argument.
def int32_arg(name_ref, value):
  return [((value & 0xffffffff) << 32) | (name_ref << 16) | (1 << 4) | 1]


def uint32_arg(name_ref, value):
  return [(value << 32) | (name_ref << 16) | (1 << 4) | 2]


def int64_arg(name_ref, value):
  return [(name_ref << 16) | (2 << 4) | 3, value & 0xffffffffffffffff]


def uint64_arg(name_ref, value):
  return [(name_ref << 16) | (2 << 4) | 4, value]


def double_arg(name_ref, value):
  bits = struct.unpack('<Q', struct.pack('<d', value))[0]
  return [(name_ref << 16) | (2 << 4) | 5, bits]


def string_arg(name_ref, value_ref):
  return [(value_ref << 32) | (name_ref << 16) | (1 << 4) | 6]


def push_counter_event(ts, args, counter_id=0):
  arg_words = [w for arg in args for w in arg]
  # Words: header, timestamp, arguments and counter id.
  num_words = 3 + len(arg_words)
  # record_type = 4 (event), event_type = 1 (counter), thread_ref = 1.
  header = (CPU_FREQUENCY << 48) | (POWER << 32) | (1 << 24) | (
      len(args) << 20) | (1 << 16) | (num_words << 4) | 4
  push_word(header)
  push_word(ts)
  for word in arg_words:
    push_word(word)
  push_word(counter_id)


def main():
  # Fuchsia trace magic word: 0x0016547846040010
  sys.stdout.buffer.write(b'\x10\x00\x04\x46\x78\x54\x16\x00')

  push_string(POWER, 'power')
  push_string(CPU_FREQUENCY, 'cpu_frequency')
  push_string(CPU, 'cpu')
  push_string(VALUE, 'value')
  push_string(CLUSTER, 'cluster')
  push_string(NOT_A_NUMBER, 'not_a_number')

  # Thread record: pid=100, tid=200.
  push_word((1 << 16) | (3 << 4) | 3)
  push_word(100)
  push_word(200)

  # Frequencies emitted as integers are ingested as kHz.
  push_counter_event(
      1000, [uint32_arg(CPU, 0), uint64_arg(VALUE, 1996800)], counter_id=0)
  push_counter_event(
      2000, [uint32_arg(CPU, 3), uint64_arg(VALUE, 2688000)], counter_id=3)
  # CPU 0 DVFS transition.
  push_counter_event(
      3000, [uint32_arg(CPU, 0), uint64_arg(VALUE, 1190400)], counter_id=0)

  # Arguments beyond "cpu" and "value" are not part of the contract and are
  # ignored rather than rejected.
  push_counter_event(
      4000,
      [uint32_arg(CPU, 5),
       uint32_arg(CLUSTER, 1),
       uint64_arg(VALUE, 2800000)],
      counter_id=5)

  # Frequencies emitted as finite, non-negative doubles are also ingested.
  push_counter_event(
      5000,
      [uint32_arg(CPU, 4), double_arg(VALUE, 1804800.0)], counter_id=4)

  # Negative, non-finite, non-numeric and ambiguous records violate the
  # contract: they are dropped and counted in the fuchsia_invalid_event stat, so
  # no track is created for CPUs 1, 2, 6, 7 and 8.
  push_counter_event(6000, [uint32_arg(CPU, 1), int32_arg(VALUE, -500)])
  push_counter_event(7000, [uint32_arg(CPU, 2), int64_arg(VALUE, -1000)])
  push_counter_event(8000, [uint32_arg(CPU, 6), double_arg(VALUE, math.nan)])
  push_counter_event(9000, [uint32_arg(CPU, 6), double_arg(VALUE, math.inf)])
  push_counter_event(10000, [uint32_arg(CPU, 6), double_arg(VALUE, -1500.0)])
  # A frequency which is not a number at all.
  push_counter_event(
      11000,
      [uint32_arg(CPU, 7), string_arg(VALUE, NOT_A_NUMBER)])
  # Repeating "value" leaves the frequency ambiguous.
  push_counter_event(12000, [
      uint32_arg(CPU, 7),
      uint64_arg(VALUE, 1000000),
      uint64_arg(VALUE, 2000000)
  ])
  # A record without a "cpu" argument names no core.
  push_counter_event(13000, [uint64_arg(VALUE, 1000000)])
  # Core indices are integers in the [0, kMaxCpusPerMachine) range.
  push_counter_event(14000, [double_arg(CPU, 8.0), uint64_arg(VALUE, 1000000)])
  push_counter_event(14500, [uint32_arg(CPU, 4096), uint64_arg(VALUE, 1000000)])
  push_counter_event(15000,
                     [uint64_arg(CPU, 2**32),
                      uint64_arg(VALUE, 1000000)])


if __name__ == '__main__':
  main()
