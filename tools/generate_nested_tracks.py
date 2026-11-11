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
"""
Script to generate a Perfetto trace binary containing deeply nested custom tracks
across multiple layers of hierarchy with flow events connecting parent and child tracks.
"""

import struct
import sys


def encode_varint(value: int) -> bytes:
  out = bytearray()
  while value >= 0x80:
    out.append((value & 0x7F) | 0x80)
    value >>= 7
  out.append(value & 0x7F)
  return bytes(out)


def encode_field(field_num: int, wire_type: int, payload: bytes) -> bytes:
  tag = (field_num << 3) | wire_type
  return encode_varint(tag) + payload


def encode_varint_field(field_num: int, value: int) -> bytes:
  return encode_field(field_num, 0, encode_varint(value))


def encode_fixed64_field(field_num: int, value: int) -> bytes:
  tag = (field_num << 3) | 1  # wire_type = 1 (64-bit fixed)
  return encode_varint(tag) + struct.pack('<Q', value)


def encode_string_field(field_num: int, value: str) -> bytes:
  data = value.encode('utf-8')
  return encode_field(field_num, 2, encode_varint(len(data)) + data)


def encode_submessage(field_num: int, data: bytes) -> bytes:
  return encode_field(field_num, 2, encode_varint(len(data)) + data)


class PerfettoTraceBuilder:

  def __init__(self, sequence_id: int = 1):
    self.sequence_id = sequence_id
    self.packets = []
    self.first_packet = True

  def _create_packet(self,
                     data_field_num: int,
                     data_bytes: bytes,
                     ts_ns: int = None) -> bytes:
    pkt = bytearray()

    # Set trusted_packet_sequence_id (field 10)
    pkt += encode_varint_field(10, self.sequence_id)

    # Set sequence_flags (field 41): SEQ_INCREMENTAL_STATE_CLEARED = 1 for the first packet
    if self.first_packet:
      pkt += encode_varint_field(41, 1)
      self.first_packet = False

    if ts_ns is not None:
      pkt += encode_varint_field(8, ts_ns)  # timestamp = field 8

    pkt += encode_submessage(data_field_num, data_bytes)
    return bytes(pkt)

  def add_track_descriptor(self, uuid: int, name: str, parent_uuid: int = None):
    """Defines a custom track and its optional parent track UUID."""
    td_bytes = bytearray()
    td_bytes += encode_varint_field(1, uuid)
    td_bytes += encode_string_field(2, name)
    if parent_uuid is not None:
      td_bytes += encode_varint_field(5, parent_uuid)

    self.packets.append(self._create_packet(
        60, bytes(td_bytes)))  # track_descriptor = field 60

  def add_slice(self,
                track_uuid: int,
                name: str,
                start_ns: int,
                dur_ns: int,
                category: str = "app",
                flow_ids=None,
                terminating_flow_ids=None):
    """Adds a slice (BEGIN + END) on the specified track, with optional flow arrows."""
    # TYPE_SLICE_BEGIN = 1
    te_begin = bytearray()
    te_begin += encode_varint_field(9, 1)
    te_begin += encode_varint_field(11, track_uuid)
    te_begin += encode_string_field(22, category)
    te_begin += encode_string_field(23, name)

    if flow_ids:
      for fid in flow_ids:
        te_begin += encode_fixed64_field(47, fid)  # flow_ids = field 47

    if terminating_flow_ids:
      for fid in terminating_flow_ids:
        te_begin += encode_fixed64_field(48,
                                         fid)  # terminating_flow_ids = field 48

    self.packets.append(
        self._create_packet(11, bytes(te_begin),
                            ts_ns=start_ns))  # track_event = field 11

    # TYPE_SLICE_END = 2
    te_end = bytearray()
    te_end += encode_varint_field(9, 2)
    te_end += encode_varint_field(11, track_uuid)

    self.packets.append(
        self._create_packet(11, bytes(te_end),
                            ts_ns=start_ns + dur_ns))  # track_event = field 11

  def add_instant(self,
                  track_uuid: int,
                  name: str,
                  ts_ns: int,
                  category: str = "app"):
    """Adds an instant event on the specified track."""
    # TYPE_INSTANT = 3
    te = bytearray()
    te += encode_varint_field(9, 3)
    te += encode_varint_field(11, track_uuid)
    te += encode_string_field(22, category)
    te += encode_string_field(23, name)

    self.packets.append(self._create_packet(11, bytes(te), ts_ns=ts_ns))

  def serialize(self) -> bytes:
    """Serializes all trace packets into a binary Perfetto trace."""
    out = bytearray()
    for pkt in self.packets:
      out += encode_submessage(1, pkt)  # Trace.packet = field 1
    return bytes(out)


def build_nested_node(builder,
                      parent_uuid,
                      current_depth,
                      max_depth,
                      branching_factor,
                      state,
                      incoming_flow_id=None):
  if current_depth > max_depth:
    return

  track_uuid = state["uuid_counter"]
  state["uuid_counter"] += 1
  state["total_tracks"] += 1

  track_name = f"L{current_depth} Track {state['total_tracks']:02d}"
  builder.add_track_descriptor(
      uuid=track_uuid, name=track_name, parent_uuid=parent_uuid)

  ts = state["ts_counter"]
  dur = 2_000_000 * (max_depth - current_depth + 1)
  state["ts_counter"] += 500_000

  # Generate dedicated flow IDs for each child node
  child_flow_ids = []
  if current_depth < max_depth:
    for _ in range(branching_factor):
      fid = state["flow_counter"]
      state["flow_counter"] += 1
      child_flow_ids.append(fid)

  term_flows = [incoming_flow_id] if incoming_flow_id is not None else None

  builder.add_slice(
      track_uuid=track_uuid,
      name=f"L{current_depth} Slice {state['total_slices'] + 1}",
      start_ns=ts,
      dur_ns=dur,
      flow_ids=child_flow_ids if child_flow_ids else None,
      terminating_flow_ids=term_flows)

  state["total_slices"] += 1
  if incoming_flow_id is not None:
    state["total_flows"] += 1

  # Recursively build child nodes, passing the respective flow ID to each child
  if current_depth < max_depth:
    for child_idx in range(branching_factor):
      build_nested_node(
          builder=builder,
          parent_uuid=track_uuid,
          current_depth=current_depth + 1,
          max_depth=max_depth,
          branching_factor=branching_factor,
          state=state,
          incoming_flow_id=child_flow_ids[child_idx])


def main():
  builder = PerfettoTraceBuilder(sequence_id=1)

  output_filename = "nested_tracks.perfetto-trace"
  max_depth = 5
  branching_factor = 4
  num_roots = 4

  if len(sys.argv) > 1:
    output_filename = sys.argv[1]
  if len(sys.argv) > 2:
    max_depth = int(sys.argv[2])
  if len(sys.argv) > 3:
    branching_factor = int(sys.argv[3])

  print(
      f"Generating trace with {num_roots} roots x {max_depth} layers of nesting (branching {branching_factor}) + flows..."
  )

  state = {
      "uuid_counter": 1000,
      "flow_counter": 5000,
      "ts_counter": 1_000_000,
      "total_tracks": 0,
      "total_slices": 0,
      "total_flows": 0
  }

  for _ in range(num_roots):
    build_nested_node(
        builder=builder,
        parent_uuid=None,
        current_depth=1,
        max_depth=max_depth,
        branching_factor=branching_factor,
        state=state)

  with open(output_filename, "wb") as f:
    f.write(builder.serialize())

  print(f"Successfully generated {output_filename}")
  print(f"Nesting Depth: {max_depth} levels")
  print(f"Total Tracks Generated: {state['total_tracks']}")
  print(f"Total Slices Generated: {state['total_slices']}")
  print(f"Total Flow Connections: {state['total_flows']}")


if __name__ == "__main__":
  main()
