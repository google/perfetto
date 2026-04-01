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

# Generates a Perfetto trace with two sequential HprofDump streams using
# the same pid. The first dump completes (last_chunk=true) before the
# second begins, testing that HprofDumpModule correctly finalizes and
# creates a new parser for the same pid.

from os import sys, path

import synth_common

HPROF_PATH = path.join(
    path.dirname(path.abspath(__file__)), '..', '..', '..', '..', 'data',
    'test-dump.hprof')
CHUNK_SIZE = 256 * 1024  # 256 KiB

PID = 1234

trace = synth_common.create_trace()

with open(HPROF_PATH, 'rb') as f:
  hprof_data = f.read()

total_chunks = (len(hprof_data) + CHUNK_SIZE - 1) // CHUNK_SIZE

# Write two complete dumps sequentially with the same pid.
for _ in range(2):
  for i in range(total_chunks):
    start = i * CHUNK_SIZE
    packet = trace.add_packet()
    packet.hprof_dump.pid = PID
    packet.hprof_dump.hprof_data = hprof_data[start:start + CHUNK_SIZE]
    packet.hprof_dump.chunk_index = i
    packet.hprof_dump.last_chunk = (i == total_chunks - 1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
