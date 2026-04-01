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

# Generates a Perfetto trace with two HprofDump streams (different pids)
# using chunked delivery. Both streams use the same hprof file but with
# different pids, and chunks are interleaved to exercise the per-pid
# reassembly in HprofDumpModule.

from os import sys, path

import synth_common

HPROF_PATH = path.join(
    path.dirname(path.abspath(__file__)), '..', '..', '..', '..', 'data',
    'test-dump.hprof')
CHUNK_SIZE = 256 * 1024  # 256 KiB

PID_A = 1000
PID_B = 2000

trace = synth_common.create_trace()

with open(HPROF_PATH, 'rb') as f:
  hprof_data = f.read()


def add_chunks(data, pid):
  """Returns a list of (pid, chunk_index, is_last, data) tuples."""
  chunks = []
  total = (len(data) + CHUNK_SIZE - 1) // CHUNK_SIZE
  for i in range(total):
    start = i * CHUNK_SIZE
    chunks.append((pid, i, i == total - 1, data[start:start + CHUNK_SIZE]))
  return chunks


chunks_a = add_chunks(hprof_data, PID_A)
chunks_b = add_chunks(hprof_data, PID_B)

# Interleave chunks from both pids: A0, B0, A1, B1, ...
i_a = 0
i_b = 0
while i_a < len(chunks_a) or i_b < len(chunks_b):
  if i_a < len(chunks_a):
    pid, idx, last, data = chunks_a[i_a]
    packet = trace.add_packet()
    packet.hprof_dump.pid = pid
    packet.hprof_dump.hprof_data = data
    packet.hprof_dump.chunk_index = idx
    packet.hprof_dump.last_chunk = last
    i_a += 1
  if i_b < len(chunks_b):
    pid, idx, last, data = chunks_b[i_b]
    packet = trace.add_packet()
    packet.hprof_dump.pid = pid
    packet.hprof_dump.hprof_data = data
    packet.hprof_dump.chunk_index = idx
    packet.hprof_dump.last_chunk = last
    i_b += 1

sys.stdout.buffer.write(trace.trace.SerializeToString())
