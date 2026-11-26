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

# Writes an ART method trace in the V2 streaming dual-clock layout produced by
# TraceWriter::FlushEntriesFormatV2() in AOSP art/runtime/trace.cc:
# each record is SLEB128(delta of method_ptr | action), ULEB128(delta of wall
# ns since start), ULEB128(delta of thread cpu ns); bytes 14..31 of the header
# are zero.

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


TID = 4242
OUTER = 0x6f8a1230  # ArtMethod pointers, 4-byte aligned like on device.
INNER = 0x6f8a1260
ENTER, EXIT = 0, 1

out = bytearray()
# Header: magic, version 0xf5 (streaming, dual clock), start time in ns.
out += struct.pack('<IHQ', 0x574f4c53, 0xf5, 1000000) + bytes(18)
name = b'main'
out += struct.pack('<BIH', 0, TID, len(name)) + name
for mid, line in ((OUTER, b'com.example.Foo\touter\t()V\tFoo.java\n'),
                  (INNER, b'com.example.Foo\tinner\t()V\tFoo.java\n')):
  out += struct.pack('<BQH', 1, mid, len(line)) + line

# (method, action, wall ns since start, thread cpu ns)
records = [(OUTER, ENTER, 100000, 50000), (INNER, ENTER, 200000, 90000),
           (INNER, EXIT, 300000, 120000), (OUTER, EXIT, 500000, 200000)]
body = bytearray()
prev_m = prev_w = prev_c = 0
for m, act, wall, cpu in records:
  body += sleb((m | act) - prev_m)
  body += uleb(wall - prev_w)
  body += uleb(cpu - prev_c)
  prev_m, prev_w, prev_c = m | act, wall, cpu
out += struct.pack('<BI', 2, TID) + len(records).to_bytes(3, 'little')
out += struct.pack('<I', len(body)) + body

summary = (b'*version\n5\ndata-file-overflow=false\nclock=dual\n'
           b'elapsed-time-nsec=600000\nvm=art\npid=4242\n*end\n')
out += struct.pack('<BH', 3, len(summary)) + summary
sys.stdout.buffer.write(out)
