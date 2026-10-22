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

import struct
import sys
from perf_thread_cpu_counters import (
    FEAT_EVENT_DESC,
    PERF_COUNT_HW_CPU_CYCLES,
    PERF_COUNT_HW_INSTRUCTIONS,
    PERF_FORMAT_GROUP,
    PERF_FORMAT_ID,
    PERF_SAMPLE_CPU,
    PERF_SAMPLE_ID,
    PERF_SAMPLE_IP,
    PERF_SAMPLE_READ,
    PERF_SAMPLE_TID,
    PERF_SAMPLE_TIME,
    PERF_TYPE_HARDWARE,
    pack_attr,
    pack_comm,
    pack_event_desc,
    pack_id_index,
    pack_sample,
)


def main():
  sample_type = (
      PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_READ
      | PERF_SAMPLE_ID | PERF_SAMPLE_CPU)
  read_format = PERF_FORMAT_ID | PERF_FORMAT_GROUP

  attr_cycles = pack_attr(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 1000,
                          sample_type, read_format)
  attr_instr = pack_attr(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,
                         1 << 63, sample_type, read_format)

  # Mode 1 (pid == -1, cpu >= 0): system-wide per-CPU counter fds
  cycles_ids = [1]
  instr_ids = [11]

  records = b''.join([
      pack_id_index([
          (1, 0, 0, -1),
          (11, 1, 0, -1),
      ]),
      pack_comm(1000, 1001, 'sys-worker-1'),
      pack_comm(1000, 1002, 'sys-worker-2'),
      # CPU 0 counter shared across tid=1001 (0 -> 1000) and tid=1002 (1000 -> 2500)
      pack_sample(0x401000, 1000, 1001, 1000, 1, 0, [(1000, 1), (2000, 11)]),
      pack_sample(0x401010, 1000, 1002, 2000, 1, 0, [(2500, 1), (5000, 11)]),
  ])

  header_size = 104
  ids_blob = struct.pack('<Q', 1) + struct.pack('<Q', 11)
  ids_off = header_size
  attrs_off = ids_off + len(ids_blob)
  attrs_blob = (
      attr_cycles + struct.pack('<QQ', ids_off, 8) + attr_instr +
      struct.pack('<QQ', ids_off + 8, 8))
  data_off = attrs_off + len(attrs_blob)
  feat_headers_off = data_off + len(records)
  event_desc_blob = pack_event_desc([
      (attr_cycles, 'cpu-cycles', cycles_ids),
      (attr_instr, 'instructions', instr_ids),
  ])
  event_desc_off = feat_headers_off + 16
  feat_headers_blob = struct.pack('<QQ', event_desc_off, len(event_desc_blob))

  header = struct.pack('<8sQQQQQQQQQQQQ', b'PERFILE2',
                       header_size, 152, attrs_off, len(attrs_blob), data_off,
                       len(records), 0, 0, 1 << FEAT_EVENT_DESC, 0, 0, 0)

  sys.stdout.buffer.write(header + ids_blob + attrs_blob + records +
                          feat_headers_blob + event_desc_blob)


if __name__ == '__main__':
  main()
