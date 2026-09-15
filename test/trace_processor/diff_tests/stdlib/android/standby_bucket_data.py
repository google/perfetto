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
"""Synthetic trace generator for StatsD App Standby Bucket atoms.

Note on why TextProto cannot be used here:
`protos/perfetto/trace/perfetto_trace.proto` defines `message Atom {}` as an empty
message (without atom fields or proto extensions). StatsD atoms are decoded
dynamically inside Trace Processor using runtime C++ descriptor tables
(`kAtomsDescriptor`). Because Python's `google.protobuf.text_format.Merge` strictly
validates fields against the compiled Python protobuf classes, specifying atom
fields in a TextProto fails at parse time with:
`ParseError: Message type "Atom" has no field named "app_standby_bucket_changed"`.
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


def build_standby_atom(package_name, bucket, reason):
  """Builds wire format for Atom 258 (AppStandbyBucketChanged).

  Wire layout:
    Atom 258 tag: (258 << 3) | 2
    Subfields:
      Field 1 (package_name): string
      Field 3 (bucket): varint (10 = ACTIVE, 20 = WORKING_SET)
      Field 4 (reason): varint
  """
  payload = (
      encode_length_delimited(1, package_name) +
      encode_varint_field(3, bucket) + encode_varint_field(4, reason))
  return encode_length_delimited(258, payload)


def main():
  # Atom 258: package="com.example.app", bucket=10 (ACTIVE), reason=0 at ts=1s
  atom1 = build_standby_atom('com.example.app', bucket=10, reason=0)
  # Atom 258: package="com.example.app", bucket=20 (WORKING_SET), reason=1 at ts=5s
  atom2 = build_standby_atom('com.example.app', bucket=20, reason=1)

  # statsd_atom (TracePacket field 84) layout in StatsdAtom:
  #   Field 1 (nested atom): Atom submessage
  #   Field 2 (atom_timestamp_nanos): int64 timestamp
  statsd_atom_payload = (
      encode_length_delimited(1, atom1) + encode_length_delimited(1, atom2) +
      encode_varint_field(2, 1000000000) + encode_varint_field(2, 5000000000))

  # TracePacket (Trace field 1) containing statsd_atom (field 84)
  packet = encode_length_delimited(84, statsd_atom_payload)
  trace = encode_length_delimited(1, packet)
  sys.stdout.buffer.write(trace)


if __name__ == '__main__':
  main()
