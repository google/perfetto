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

# Writes an ART method v2 single-clock thread-cpu trace: ART writes
# tsc_frequency == 1 (while still populating start_tsc with the wall TSC) and
# encodes per-thread CPU nanoseconds starting at 0 in each record.

import struct
import sys


def sleb(v):
  out = bytearray()
  while True:
    b = v & 0x7f
    v >>= 7
    if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
      out.append(b)
      return bytes(out)
    out.append(b | 0x80)


TID = 1440
OUTER = 0x70001000
INNER = 0x70001040
ENTER, EXIT = 0, 1

START_TS_NS = 170_000_000_000
START_TSC = 448_445_205_368

out = bytearray()
out += struct.pack('<IHQQQ', 0x574f4c53, 0x04, START_TS_NS, START_TSC,
                   1) + bytes(2)

name = b'main'
out += struct.pack('<BIH', 0, TID, len(name)) + name

for mid, line in (
    (OUTER, b'android.app.ProfilerInfo\tcloseFd\t()V\tProfilerInfo.java\n'),
    (INNER, b'android.os.ParcelFileDescriptor\tclose\t()V\t'
     b'ParcelFileDescriptor.java\n'),
):
  out += struct.pack('<BQH', 1, mid, len(line)) + line

records = [
    (0, ENTER, OUTER),
    (50_000, ENTER, INNER),
    (120_000, EXIT, None),
    (142_402, EXIT, None),
]

body = bytearray()
prev_ts_act = 0
prev_m = 0
for cpu_ns, act, mid in records:
  ts_act = (cpu_ns << 2) | act
  body += sleb(ts_act - prev_ts_act)
  prev_ts_act = ts_act
  if act == ENTER:
    body += sleb(mid - prev_m)
    prev_m = mid

out += struct.pack('<BI', 2, TID) + len(records).to_bytes(3, 'little')
out += struct.pack('<I', len(body)) + body

summary = (b'*version\n4\ndata-file-overflow=false\nclock=thread-cpu\n'
           b'elapsed-time-nsec=1000000\nvm=art\npid=1440\n'
           b'is_streaming=true\nis_precise_trace=true\n*end\n')
out += struct.pack('<BH', 3, len(summary)) + summary
sys.stdout.buffer.write(out)
