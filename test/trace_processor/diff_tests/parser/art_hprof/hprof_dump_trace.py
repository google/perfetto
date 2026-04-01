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

# Generates a Perfetto trace containing a single HprofDump packet that
# embeds the contents of test-dump.hprof.

from os import sys, path

import synth_common

HPROF_PATH = path.join(
    path.dirname(path.abspath(__file__)), '..', '..', '..', '..', 'data',
    'test-dump.hprof')

trace = synth_common.create_trace()

with open(HPROF_PATH, 'rb') as f:
  packet = trace.add_packet()
  packet.hprof_dump.pid = 1234
  packet.hprof_dump.hprof_data = f.read()
  packet.hprof_dump.chunk_index = 0
  packet.hprof_dump.last_chunk = True

sys.stdout.buffer.write(trace.trace.SerializeToString())
