# Copyright (C) 2024 The Android Open Source Project
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
"""Synthetic trace generator for StatsD App Freezer (AppFreezeChanged) atoms.

Note on why TextProto cannot be used here:
`protos/perfetto/trace/perfetto_trace.proto` defines `message Atom {}` as an empty
message (without atom fields or proto extensions). StatsD atoms are decoded
dynamically inside Trace Processor using runtime C++ descriptor tables
(`kAtomsDescriptor`). Because Python's `google.protobuf.text_format.Merge` strictly
validates fields against the compiled Python protobuf classes, specifying atom
fields in a TextProto fails at parse time with:
`ParseError: Message type "Atom" has no field named "app_freeze_changed"`.
Hence, we construct the protobuf wire bytes directly from primitives.
"""

import sys


def encode_varint(n):
  res = bytearray()
  while True:
    b = n & 0x7F
    n >>= 7
    if n:
      res.append(b | 0x80)
    else:
      res.append(b)
      break
  return bytes(res)


def encode_varint_field(field_num, val):
  tag = (field_num << 3) | 0
  return encode_varint(tag) + encode_varint(val)


def encode_length_delimited(field_num, data):
  tag = (field_num << 3) | 2
  if isinstance(data, str):
    data = data.encode('utf-8')
  return encode_varint(tag) + encode_varint(len(data)) + data


def build_app_freeze_atom(state, pid, process_name, uid, frozen_reason):
  """Builds wire format for Atom 254 (AppFreezeChanged).

  Wire layout:
    Atom 254 tag: (254 << 3) | 2
    Subfields:
      Field 1 (state): varint (1 = FROZEN, 2 = UNFROZEN)
      Field 2 (pid): varint
      Field 3 (process_name): string
      Field 4 (uid): varint
      Field 6 (frozen_reason): varint
  """
  payload = (
      encode_varint_field(1, state) + encode_varint_field(2, pid) +
      encode_length_delimited(3, process_name) + encode_varint_field(4, uid) +
      encode_varint_field(6, frozen_reason))
  return encode_length_delimited(254, payload)


def main():
  # Atom 254: state=1 (FROZEN), pid=1234, name="com.example.app", uid=0, reason=0 at ts=1s
  atom1 = build_app_freeze_atom(
      state=1,
      pid=1234,
      process_name='com.example.app',
      uid=0,
      frozen_reason=0,
  )
  # Atom 254: state=2 (UNFROZEN), pid=1234, name="com.example.app", uid=4, reason=1 at ts=5s
  atom2 = build_app_freeze_atom(
      state=2,
      pid=1234,
      process_name='com.example.app',
      uid=4,
      frozen_reason=1,
  )

  # TracePacket 1: statsd_atom with atom1 at ts=1s
  payload1 = encode_length_delimited(1, atom1) + encode_varint_field(
      2, 1000000000)
  packet1 = encode_length_delimited(84, payload1)

  # TracePacket 2: statsd_atom with atom2 at ts=5s
  payload2 = encode_length_delimited(1, atom2) + encode_varint_field(
      2, 5000000000)
  packet2 = encode_length_delimited(84, payload2)

  # Trace containing repeated TracePackets (field 1)
  trace = encode_length_delimited(1, packet1) + encode_length_delimited(
      1, packet2)
  sys.stdout.buffer.write(trace)


if __name__ == '__main__':
  main()
