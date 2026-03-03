#!/usr/bin/env python3
"""Extract TracePackets containing AppWakelockBundle from a trace file.

Also de-interns the data, producing a second trace where each wakelock event
is a standalone TrackEvent with AppWakelockInfo embedded directly.

Usage:
  python3 tools/wakelock_extract.py long_trace.pftrace
  # Produces: wakelock.pftrace (interned packets only)
  #           wakelock_uninterned.pftrace (de-interned as TrackEvents)
"""

import struct
import sys
import os

# ---- Low-level protobuf wire format helpers ----


def encode_varint(value):
  """Encode an unsigned integer as a varint."""
  buf = b''
  while value > 0x7F:
    buf += bytes([0x80 | (value & 0x7F)])
    value >>= 7
  buf += bytes([value & 0x7F])
  return buf


def decode_varint(data, pos):
  """Decode a varint from data at pos. Returns (value, new_pos)."""
  result = 0
  shift = 0
  while True:
    b = data[pos]
    result |= (b & 0x7F) << shift
    pos += 1
    if not (b & 0x80):
      break
    shift += 7
  return result, pos


def encode_field_varint(field_num, value):
  """Encode a varint field."""
  tag = (field_num << 3) | 0  # wire type 0 = varint
  return encode_varint(tag) + encode_varint(value)


def encode_field_signed_varint(field_num, value):
  """Encode a signed varint field (using two's complement for negatives)."""
  if value < 0:
    value = value + (1 << 64)
  return encode_field_varint(field_num, value)


def encode_field_bytes(field_num, data):
  """Encode a length-delimited field."""
  tag = (field_num << 3) | 2  # wire type 2 = length-delimited
  return encode_varint(tag) + encode_varint(len(data)) + data


def encode_field_string(field_num, s):
  """Encode a string field."""
  return encode_field_bytes(field_num, s.encode('utf-8'))


def zigzag_encode(value):
  """Encode a signed int as a zigzag varint."""
  return (value << 1) ^ (value >> 63)


def zigzag_decode(value):
  """Decode a zigzag varint to a signed int."""
  return (value >> 1) ^ -(value & 1)


def parse_proto_fields(data):
  """Parse protobuf fields from raw bytes. Yields (field_num, wire_type, value)."""
  pos = 0
  while pos < len(data):
    tag, pos = decode_varint(data, pos)
    field_num = tag >> 3
    wire_type = tag & 0x7
    if wire_type == 0:  # varint
      value, pos = decode_varint(data, pos)
      yield field_num, wire_type, value
    elif wire_type == 2:  # length-delimited
      length, pos = decode_varint(data, pos)
      value = data[pos:pos + length]
      pos += length
      yield field_num, wire_type, value
    elif wire_type == 5:  # 32-bit
      value = data[pos:pos + 4]
      pos += 4
      yield field_num, wire_type, value
    elif wire_type == 1:  # 64-bit
      value = data[pos:pos + 8]
      pos += 8
      yield field_num, wire_type, value
    else:
      raise ValueError(f"Unknown wire type {wire_type} at pos {pos}")


def parse_packed_varints(data):
  """Parse packed repeated varints from bytes."""
  values = []
  pos = 0
  while pos < len(data):
    v, pos = decode_varint(data, pos)
    values.append(v)
  return values


def parse_trace_packets(data):
  """Parse a trace file into individual TracePacket raw bytes.
    A Perfetto trace is a sequence of field-1 (Trace.packet) entries."""
  pos = 0
  while pos < len(data):
    tag, pos = decode_varint(data, pos)
    field_num = tag >> 3
    wire_type = tag & 0x7
    assert wire_type == 2 and field_num == 1, \
        f"Expected Trace.packet (field 1, wire type 2), got field={field_num}, wire_type={wire_type}"
    length, pos = decode_varint(data, pos)
    packet_bytes = data[pos:pos + length]
    pos += length
    yield packet_bytes


def write_trace_packet(f, packet_bytes):
  """Write a single TracePacket to a trace file (as Trace.packet field)."""
  f.write(encode_field_bytes(1, packet_bytes))


# ---- Field number constants ----

# TracePacket fields
TP_TIMESTAMP = 8
TP_TIMESTAMP_CLOCK_ID = 58
TP_TRUSTED_PACKET_SEQUENCE_ID = 10
TP_SEQUENCE_FLAGS = 13
TP_INTERNED_DATA = 12
TP_APP_WAKELOCK_BUNDLE = 116
TP_TRACK_EVENT = 11
TP_TRACK_DESCRIPTOR = 60

# AppWakelockBundle fields
AWB_INTERN_ID = 1
AWB_ENCODED_TS = 2
AWB_INFO = 3
AWB_ACQUIRED = 4

# AppWakelockInfo fields
AWI_IID = 1
AWI_TAG = 2
AWI_FLAGS = 3
AWI_OWNER_PID = 4
AWI_OWNER_UID = 5
AWI_WORK_UID = 6

# InternedData fields
ID_APP_WAKELOCK_INFO = 42

# TrackEvent fields
TE_TYPE = 9
TE_TRACK_UUID = 11
TE_NAME = 23
TE_APP_WAKELOCK_INFO = 57  # Added to track_event.proto

# TrackEvent.Type
TE_TYPE_SLICE_BEGIN = 1
TE_TYPE_SLICE_END = 2
TE_TYPE_INSTANT = 3

# TrackDescriptor fields
TD_UUID = 1
TD_NAME = 2


def parse_app_wakelock_info(data):
  """Parse an AppWakelockInfo message. Returns dict of fields."""
  info = {}
  for field_num, wire_type, value in parse_proto_fields(data):
    if field_num == AWI_IID:
      info['iid'] = value
    elif field_num == AWI_TAG:
      info['tag'] = value.decode('utf-8') if isinstance(value, bytes) else value
    elif field_num == AWI_FLAGS:
      info['flags'] = value
    elif field_num == AWI_OWNER_PID:
      info['owner_pid'] = value
    elif field_num == AWI_OWNER_UID:
      info['owner_uid'] = value
    elif field_num == AWI_WORK_UID:
      info['work_uid'] = value
  return info


def serialize_app_wakelock_info_no_iid(info):
  """Serialize AppWakelockInfo without the iid field."""
  buf = b''
  if 'tag' in info:
    buf += encode_field_string(AWI_TAG, info['tag'])
  if 'flags' in info:
    buf += encode_field_varint(AWI_FLAGS, info['flags'])
  if 'owner_pid' in info:
    buf += encode_field_varint(AWI_OWNER_PID, info['owner_pid'])
  if 'owner_uid' in info:
    buf += encode_field_varint(AWI_OWNER_UID, info['owner_uid'])
  if 'work_uid' in info:
    buf += encode_field_varint(AWI_WORK_UID, info['work_uid'])
  return buf


def main():
  if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <input.pftrace>", file=sys.stderr)
    sys.exit(1)

  input_path = sys.argv[1]
  base_dir = os.path.dirname(input_path) or '.'
  wakelock_path = os.path.join(base_dir, 'wakelock.pftrace')
  uninterned_path = os.path.join(base_dir, 'wakelock_uninterned.pftrace')

  print(f"Reading {input_path}...")
  with open(input_path, 'rb') as f:
    trace_data = f.read()
  print(f"  Read {len(trace_data)} bytes")

  # First pass: extract packets with AppWakelockBundle, build intern table
  wakelock_packets = []
  intern_table = {}  # iid -> AppWakelockInfo dict
  total_packets = 0
  wakelock_event_count = 0
  seq_id_to_interns = {}  # per-sequence interning

  for packet_bytes in parse_trace_packets(trace_data):
    total_packets += 1
    has_wakelock_bundle = False
    packet_ts = None
    packet_seq_id = None
    interned_infos = []

    for field_num, wire_type, value in parse_proto_fields(packet_bytes):
      if field_num == TP_APP_WAKELOCK_BUNDLE:
        has_wakelock_bundle = True
      elif field_num == TP_TIMESTAMP:
        packet_ts = value
      elif field_num == TP_TRUSTED_PACKET_SEQUENCE_ID:
        packet_seq_id = value
      elif field_num == TP_INTERNED_DATA:
        # Parse interned data for AppWakelockInfo entries
        for id_field_num, id_wire_type, id_value in parse_proto_fields(value):
          if id_field_num == ID_APP_WAKELOCK_INFO:
            info = parse_app_wakelock_info(id_value)
            interned_infos.append(info)

    if interned_infos:
      if packet_seq_id not in seq_id_to_interns:
        seq_id_to_interns[packet_seq_id] = {}
      for info in interned_infos:
        if 'iid' in info:
          seq_id_to_interns[packet_seq_id][info['iid']] = info
          intern_table[info['iid']] = info

    if has_wakelock_bundle:
      wakelock_packets.append((packet_bytes, packet_ts, packet_seq_id))

  print(f"  Total packets: {total_packets}")
  print(f"  Packets with AppWakelockBundle: {len(wakelock_packets)}")
  print(f"  Total interned AppWakelockInfo entries: {len(intern_table)}")

  # Write wakelock.pftrace (just the wakelock packets, including their interned data)
  # We need to include all interned data that the wakelock packets reference.
  # The simplest approach: write the packets as-is (they already contain the interned data).
  print(f"\nWriting {wakelock_path}...")
  with open(wakelock_path, 'wb') as f:
    for packet_bytes, _, _ in wakelock_packets:
      write_trace_packet(f, packet_bytes)
  wakelock_size = os.path.getsize(wakelock_path)
  print(f"  Written {wakelock_size} bytes")

  # Second pass: create de-interned trace
  # For each AppWakelockBundle, expand each (encoded_ts, intern_id) pair into
  # a separate TrackEvent with the AppWakelockInfo embedded directly.
  print(f"\nCreating de-interned trace {uninterned_path}...")

  TRACK_UUID = 0x57414B454C4F434B  # "WAKELOCK" in hex, arbitrary UUID

  with open(uninterned_path, 'wb') as f:
    # First, write a TrackDescriptor for the wakelock track
    td_bytes = b''
    td_bytes += encode_field_varint(TD_UUID, TRACK_UUID)
    td_bytes += encode_field_string(TD_NAME, 'app_wakelock_events')

    pkt_bytes = b''
    pkt_bytes += encode_field_bytes(TP_TRACK_DESCRIPTOR, td_bytes)
    pkt_bytes += encode_field_varint(TP_TRUSTED_PACKET_SEQUENCE_ID, 1)
    # SEQ_INCREMENTAL_STATE_CLEARED = 2
    pkt_bytes += encode_field_varint(TP_SEQUENCE_FLAGS, 2)
    write_trace_packet(f, pkt_bytes)

    total_events = 0

    for packet_bytes, packet_ts, packet_seq_id in wakelock_packets:
      # Get the intern table for this sequence
      seq_interns = seq_id_to_interns.get(packet_seq_id, intern_table)

      for field_num, wire_type, value in parse_proto_fields(packet_bytes):
        if field_num != TP_APP_WAKELOCK_BUNDLE:
          continue

        # Parse the bundle
        intern_ids = []
        encoded_timestamps = []
        for b_field_num, b_wire_type, b_value in parse_proto_fields(value):
          if b_field_num == AWB_INTERN_ID:
            if isinstance(b_value, bytes):
              intern_ids = parse_packed_varints(b_value)
            else:
              intern_ids.append(b_value)
          elif b_field_num == AWB_ENCODED_TS:
            if isinstance(b_value, bytes):
              encoded_timestamps = parse_packed_varints(b_value)
            else:
              encoded_timestamps.append(b_value)

        if len(intern_ids) != len(encoded_timestamps):
          print(f"  WARNING: intern_ids ({len(intern_ids)}) != "
                f"encoded_ts ({len(encoded_timestamps)})")
          continue

        for iid, enc_ts in zip(intern_ids, encoded_timestamps):
          real_ts = (packet_ts or 0) + (enc_ts >> 1)
          acquired = bool(enc_ts & 1)

          info = seq_interns.get(iid)
          if info is None:
            print(f"  WARNING: Unknown intern id {iid}")
            continue

          # Build TrackEvent
          te_bytes = b''
          if acquired:
            te_bytes += encode_field_varint(TE_TYPE, TE_TYPE_SLICE_BEGIN)
          else:
            te_bytes += encode_field_varint(TE_TYPE, TE_TYPE_SLICE_END)
          te_bytes += encode_field_varint(TE_TRACK_UUID, TRACK_UUID)
          if 'tag' in info:
            te_bytes += encode_field_string(TE_NAME, info['tag'])

          # Embed AppWakelockInfo (without iid) as field 57
          wl_info_bytes = serialize_app_wakelock_info_no_iid(info)
          te_bytes += encode_field_bytes(TE_APP_WAKELOCK_INFO, wl_info_bytes)

          # Build TracePacket
          pkt = b''
          pkt += encode_field_varint(TP_TIMESTAMP, real_ts)
          pkt += encode_field_bytes(TP_TRACK_EVENT, te_bytes)
          # trusted_packet_sequence_id is required
          pkt += encode_field_varint(TP_TRUSTED_PACKET_SEQUENCE_ID, 1)

          write_trace_packet(f, pkt)
          total_events += 1

  uninterned_size = os.path.getsize(uninterned_path)
  print(f"  Written {uninterned_size} bytes ({total_events} events)")

  print(f"\nSummary:")
  print(f"  wakelock.pftrace:            {wakelock_size:>10,} bytes")
  print(f"  wakelock_uninterned.pftrace: {uninterned_size:>10,} bytes")
  print(f"  Ratio: {uninterned_size/wakelock_size:.2f}x")


if __name__ == '__main__':
  main()
