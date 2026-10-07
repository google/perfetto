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
PERF_COUNT_HW_INSTRUCTIONS = 1

PERF_SAMPLE_IP = 1 << 0
PERF_SAMPLE_TID = 1 << 1
PERF_SAMPLE_TIME = 1 << 2
PERF_SAMPLE_READ = 1 << 4
PERF_SAMPLE_ID = 1 << 6
PERF_SAMPLE_CPU = 1 << 7

PERF_FORMAT_ID = 1 << 2
PERF_FORMAT_GROUP = 1 << 3

PERF_RECORD_COMM = 3
PERF_RECORD_SAMPLE = 9
PERF_RECORD_ID_INDEX = 69

FEAT_EVENT_DESC = 12


def pack_attr(ev_type, config, period, sample_type, read_format):
  # 136-byte perf_event_attr with default perf clock (use_clockid=0)
  buf = bytearray(136)
  bitfields = 0
  struct.pack_into('<IIQQQQQ', buf, 0, ev_type, 136, config, period,
                   sample_type, read_format, bitfields)
  return bytes(buf)


def pack_record(rec_type, payload, misc=2):
  header = struct.pack('<IHH', rec_type, misc, 8 + len(payload))
  return header + payload


def pack_comm(pid, tid, name):
  name_bytes = name.encode('utf-8') + b'\x00'
  pad = (8 - (len(name_bytes) % 8)) % 8
  payload = struct.pack('<II', pid, tid) + name_bytes + (b'\x00' * pad)
  return pack_record(PERF_RECORD_COMM, payload)


def pack_id_index(entries):
  # entries: list of (id, idx, cpu, tid)
  payload = struct.pack('<Q', len(entries))
  for eid, idx, cpu, tid in entries:
    payload += struct.pack('<QQqq', eid, idx, cpu, tid)
  return pack_record(PERF_RECORD_ID_INDEX, payload, misc=0)


def pack_sample(ip, pid, tid, time_ns, sample_id, cpu, read_entries):
  payload = struct.pack('<QIIQQIIQ', ip, pid, tid, time_ns, sample_id, cpu, 0,
                        len(read_entries))
  for val, eid in read_entries:
    payload += struct.pack('<QQ', val, eid)
  return pack_record(PERF_RECORD_SAMPLE, payload)


def pack_event_desc(attrs_and_names_ids):
  out = struct.pack('<II', len(attrs_and_names_ids), 136)
  for attr_bytes, name, ids in attrs_and_names_ids:
    s = name.encode('utf-8') + b'\x00'
    pad = (4 - (len(s) % 4)) % 4
    s += b'\x00' * pad
    out += attr_bytes
    out += struct.pack('<II', len(ids), len(s))
    out += s
    for eid in ids:
      out += struct.pack('<Q', eid)
  return out


def main():
  sample_type = (
      PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_READ
      | PERF_SAMPLE_ID | PERF_SAMPLE_CPU)
  read_format = PERF_FORMAT_ID | PERF_FORMAT_GROUP

  attr_cycles = pack_attr(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, 1000,
                          sample_type, read_format)
  attr_instr = pack_attr(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,
                         1 << 63, sample_type, read_format)

  # Mode 3 (pid >= 0, cpu >= 0):
  # tid=1001 on cpu=0 (ids 1, 11), tid=1001 on cpu=1 (ids 2, 12)
  # tid=1002 on cpu=1 (ids 3, 13)
  cycles_ids = [1, 2, 3]
  instr_ids = [11, 12, 13]

  records = b''.join([
      pack_id_index([
          (1, 0, 0, 1001),
          (2, 0, 1, 1001),
          (3, 0, 1, 1002),
          (11, 1, 0, 1001),
          (12, 1, 1, 1001),
          (13, 1, 1, 1002),
      ]),
      pack_comm(1000, 1001, 'worker-1'),
      pack_comm(1000, 1002, 'worker-2'),
      # Sample 1: tid=1001 on CPU 0 -> cycles=1000, instr=2000
      pack_sample(0x401000, 1000, 1001, 1000, 1, 0, [(1000, 1), (2000, 11)]),
      # Sample 2: tid=1001 migrates to CPU 1 (new fd baseline 0) -> cycles=400, instr=800
      pack_sample(0x401010, 1000, 1001, 2000, 2, 1, [(400, 2), (800, 12)]),
      # Sample 3: tid=1002 on CPU 1 -> cycles=500, instr=900
      pack_sample(0x401020, 1000, 1002, 2500, 3, 1, [(500, 3), (900, 13)]),
      # Sample 4: tid=1001 migrates back to CPU 0 (continues from 1000/2000) -> cycles=1600, instr=3200
      pack_sample(0x401030, 1000, 1001, 3000, 1, 0, [(1600, 1), (3200, 11)]),
  ])

  header_size = 104
  ids_blob = struct.pack('<3Q', *cycles_ids) + struct.pack('<3Q', *instr_ids)
  ids_off = header_size
  attrs_off = ids_off + len(ids_blob)
  attrs_blob = (
      attr_cycles + struct.pack('<QQ', ids_off, 24) + attr_instr +
      struct.pack('<QQ', ids_off + 24, 24))
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
