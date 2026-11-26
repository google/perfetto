#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Writes an ART method v2 dual-clock trace with raw TSC timestamps, a realistic
# non-zero start_tsc (where raw_tsc * 1e9 overflows int64), a 10s span (where
# delta_tsc * 1e9 also overflows int64), ULEB128 thread-CPU deltas, and a
# 3-byte summary header (opcode + uint16 length).

import struct
import sys


def uleb(v):
  out = bytearray()
  while True:
    b = v & 0x7f
    v >>= 7
    if v:
      out.append(b | 0x80)
    else:
      out.append(b)
      return bytes(out)


def sleb(v):
  out = bytearray()
  while True:
    b = v & 0x7f
    v >>= 7
    if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
      out.append(b)
      return bytes(out)
    out.append(b | 0x80)


TID = 21396
OUTER = 0x6f8a1230
INNER = 0x6f8a1260
ENTER, EXIT_NORMAL, EXIT_UNROLL = 0, 1, 2

START_TS_NS = 165_000_000_000
START_TSC = 436_229_334_220
TSC_FREQ = 2_500_000_000  # 2.5 GHz -> 2.5 ticks/ns

out = bytearray()
out += struct.pack('<IHQQQ', 0x574f4c53, 0x05, START_TS_NS, START_TSC,
                   TSC_FREQ) + bytes(2)

name = b'main'
out += struct.pack('<BIH', 0, TID, len(name)) + name

for mid, line in (
    (OUTER, b'com.android.internal.os.RuntimeInit$MethodAndArgsCaller\trun\t'
     b'()V\tRuntimeInit.java\t548\n'),
    (INNER, b'java.lang.reflect.Method\tinvoke\t'
     b'(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;\t'
     b'Method.java\t598\n'),
):
  out += struct.pack('<BQH', 1, mid, len(line)) + line

# (raw_tsc, action, thread_cpu_ns, method_id_or_none)
records = [
    (START_TSC + 250_000, ENTER, 500, OUTER),  # +100 us
    (START_TSC + 500_000, ENTER, 600, INNER),  # +200 us
    (START_TSC + 750_000, EXIT_NORMAL, 800, None),  # +300 us (dur 100 us)
    (START_TSC + 25_000_250_000, EXIT_UNROLL, 1000,
     None),  # +10.0001 s (dur 10 s)
]

body = bytearray()
prev_ts_act = 0
prev_cpu = 0
prev_m = 0
for tsc, act, cpu, mid in records:
  ts_act = (tsc << 2) | act
  body += sleb(ts_act - prev_ts_act)
  prev_ts_act = ts_act
  body += uleb(cpu - prev_cpu)
  prev_cpu = cpu
  if act == ENTER:
    body += sleb(mid - prev_m)
    prev_m = mid

out += struct.pack('<BI', 2, TID) + len(records).to_bytes(3, 'little')
out += struct.pack('<I', len(body)) + body

summary = (b'*version\n5\ndata-file-overflow=false\nclock=dual\n'
           b'elapsed-time-nsec=10001000000\nnum-method-calls=4\n'
           b'clock-call-overhead-nsec=272\nvm=art\npid=21396\n'
           b'is_streaming=true\nis_precise_trace=true\n'
           b'end_time=175001000000\nend_tsc=461231834220\n'
           b'compilation-filter=verify\n*end\n')
out += struct.pack('<BH', 3, len(summary)) + summary
sys.stdout.buffer.write(out)
