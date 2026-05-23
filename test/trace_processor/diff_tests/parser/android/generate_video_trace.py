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
"""Generates a test trace with VideoFrame packets.

Produces two streams ("Front Camera" and "Rear Camera") interleaved by
timestamp, each with colored JPEG frames suitable for filmstrip testing.

Usage:
  python3 generate_video_trace.py [output.pb] [frames_per_stream] [fps]
"""

import io
import subprocess
import sys


def encode_varint(value):
  b = io.BytesIO()
  while value > 0x7F:
    b.write(bytes([0x80 | (value & 0x7F)]))
    value >>= 7
  b.write(bytes([value & 0x7F]))
  return b.getvalue()


def varint_field(field_num, value):
  tag = (field_num << 3) | 0
  return encode_varint(tag) + encode_varint(value)


def bytes_field(field_num, data):
  if isinstance(data, str):
    data = data.encode('utf-8')
  tag = (field_num << 3) | 2
  return encode_varint(tag) + encode_varint(len(data)) + data


def make_jpeg(frame_num, total, label, hue_offset):
  hue = (int(frame_num / max(total, 1) * 360) + hue_offset) % 360
  r = subprocess.run(
      [
          'convert',
          '-size',
          '320x180',
          'xc:hsl(%d,60%%,90%%)' % hue,
          '-gravity',
          'center',
          '-pointsize',
          '48',
          '-fill',
          'white',
          '-annotate',
          '0',
          str(frame_num),
          '-gravity',
          'south',
          '-pointsize',
          '14',
          '-annotate',
          '0',
          '%s - Frame %d' % (label, frame_num),
          'jpeg:-',
      ],
      capture_output=True,
      timeout=10,
  )
  if r.returncode != 0:
    raise RuntimeError('ImageMagick convert failed: ' + r.stderr.decode())
  return r.stdout


def make_video_frame(frame_num, jpeg):
  """Build a VideoFrame proto message."""
  msg = varint_field(1, frame_num)  # frame_number
  msg += bytes_field(2, jpeg)  # jpg_image
  return msg


def make_track_descriptor_packet(uuid, name, seq_id):
  """Build a TracePacket with a TrackDescriptor."""
  # TrackDescriptor: uuid (field 1), name (field 2)
  td = varint_field(1, uuid)
  td += bytes_field(2, name)
  # TracePacket: trusted_packet_sequence_id (field 10),
  #              track_descriptor (field 60)
  pkt = varint_field(10, seq_id)
  pkt += bytes_field(60, td)
  return bytes_field(1, pkt)


def make_trace_packet(ts, video_frame_bytes, track_uuid, seq_id):
  """Build a TracePacket with a TrackEvent containing video_frame."""
  # TrackEvent: type=INSTANT (field 9, value 3), track_uuid (field 11),
  #             video_frame (field 57)
  te = varint_field(9, 3)  # TYPE_INSTANT
  te += varint_field(11, track_uuid)
  te += bytes_field(57, video_frame_bytes)
  # TracePacket: timestamp (field 8), timestamp_clock_id (field 58),
  #              trusted_packet_sequence_id (field 10),
  #              track_event (field 11)
  pkt = varint_field(8, ts)
  pkt += varint_field(58, 6)  # BOOTTIME
  pkt += varint_field(10, seq_id)
  pkt += bytes_field(11, te)
  return bytes_field(1, pkt)


SEQ_ID = 1


def main():
  output = sys.argv[1] if len(sys.argv) > 1 else '/tmp/video_trace.pb'
  frames_per_stream = int(sys.argv[2]) if len(sys.argv) > 2 else 30
  fps = int(sys.argv[3]) if len(sys.argv) > 3 else 30
  interval_ns = 1_000_000_000 // fps
  start_ts = 1_000_000_000

  streams = [
      {
          'track_uuid': 100,
          'track_name': 'Front Camera',
          'hue_offset': 0
      },
      {
          'track_uuid': 200,
          'track_name': 'Rear Camera',
          'hue_offset': 180
      },
  ]

  # Build all frames with timestamps, then sort by ts for interleaving.
  events = []
  for s in streams:
    for i in range(frames_per_stream):
      ts = start_ts + i * interval_ns
      events.append((ts, i, s))

  events.sort(key=lambda e: (e[0], e[2]['track_uuid']))

  trace = io.BytesIO()

  # Emit one track descriptor per stream.
  for s in streams:
    trace.write(
        make_track_descriptor_packet(s['track_uuid'], s['track_name'], SEQ_ID))

  for ts, frame_num, s in events:
    jpeg = make_jpeg(frame_num, frames_per_stream, s['track_name'],
                     s['hue_offset'])
    vf = make_video_frame(frame_num, jpeg)
    trace.write(make_trace_packet(ts, vf, s['track_uuid'], SEQ_ID))
    if frame_num % 10 == 0 and s['track_uuid'] == streams[0]['track_uuid']:
      print('Frame %d/%d per stream (%d bytes)' %
            (frame_num + 1, frames_per_stream, len(jpeg)))

  data = trace.getvalue()
  with open(output, 'wb') as f:
    f.write(data)
  total = len(streams) * frames_per_stream
  print('Wrote %d frames (%d streams x %d) = %d bytes to %s' %
        (total, len(streams), frames_per_stream, len(data), output))


if __name__ == '__main__':
  main()
