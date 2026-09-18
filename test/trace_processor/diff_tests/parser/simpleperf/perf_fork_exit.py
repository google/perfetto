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

PERF_TYPE_HARDWARE = 0
PERF_COUNT_HW_CPU_CYCLES = 0

PERF_SAMPLE_IP = 1 << 0
PERF_SAMPLE_TID = 1 << 1
PERF_SAMPLE_TIME = 1 << 2

PERF_RECORD_COMM = 3
PERF_RECORD_EXIT = 4
PERF_RECORD_FORK = 7


def pack_attr(ev_type, config, period, sample_type):
  buf = bytearray(136)
  bitfields = 0
  struct.pack_into('<IIQQQQQ', buf, 0, ev_type, 136, config, period,
                   sample_type, 0, bitfields)
  return bytes(buf)


def pack_record(rec_type, payload, misc=2):
  header = struct.pack('<IHH', rec_type, misc, 8 + len(payload))
  return header + payload


def pack_comm(pid, tid, name):
  name_bytes = name.encode('utf-8') + b'\x00'
  pad = (8 - (len(name_bytes) % 8)) % 8
  payload = struct.pack('<II', pid, tid) + name_bytes + (b'\x00' * pad)
  return pack_record(PERF_RECORD_COMM, payload)


def pack_fork_or_exit(rec_type, pid, ppid, tid, ptid, time_ns):
  payload = struct.pack('<IIIIQ', pid, ppid, tid, ptid, time_ns)
  return pack_record(rec_type, payload)


def main():
  sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME
  attr = pack_attr(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 1000,
                   sample_type)

  records = b''.join([
      pack_fork_or_exit(PERF_RECORD_FORK, 1000, 1, 1001, 1000, 1000),
      pack_comm(1000, 1001, 'worker-v1'),
      pack_fork_or_exit(PERF_RECORD_EXIT, 1000, 1, 1001, 1000, 5000),
      pack_fork_or_exit(PERF_RECORD_FORK, 2000, 1000, 1001, 2000, 6000),
      pack_comm(2000, 1001, 'worker-v2'),
  ])

  header_size = 104
  ids_blob = struct.pack('<Q', 1)
  ids_off = header_size
  attrs_off = ids_off + len(ids_blob)
  attrs_blob = attr + struct.pack('<QQ', ids_off, len(ids_blob))
  data_off = attrs_off + len(attrs_blob)

  header = struct.pack('<8sQQQQQQQQQQQQ', b'PERFILE2',
                       header_size, 152, attrs_off, len(attrs_blob), data_off,
                       len(records), 0, 0, 0, 0, 0, 0)

  sys.stdout.buffer.write(header + ids_blob + attrs_blob + records)


if __name__ == '__main__':
  main()
