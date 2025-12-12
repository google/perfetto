# Copyright (C) 2025 The Android Open Source Project
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
"""Provides builders for creating Perfetto traces programmatically.

This module contains two primary classes for building Perfetto traces:
- TraceProtoBuilder: An in-memory builder suitable for smaller traces.
- StreamingTraceProtoBuilder: A streaming builder for creating large traces
  without high memory usage.

Example usage for TraceProtoBuilder:
  builder = TraceProtoBuilder()
  packet = builder.add_packet()
  packet.timestamp = 1000
  # ... populate packet ...
  trace_bytes = builder.serialize()

Example usage for StreamingTraceProtoBuilder:
  with open('trace.pftrace', 'wb') as f:
    builder = StreamingTraceProtoBuilder(f)
    packet = builder.create_packet()
    packet.timestamp = 1000
    # ... populate packet ...
    builder.write_packet(packet)
"""

from typing import IO

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace
from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import TracePacket


class TraceProtoBuilder:
  """An in-memory builder for creating Perfetto traces from Python.

  This class constructs an entire Perfetto trace in memory. It is convenient
  for smaller traces where memory consumption is not a concern. For generating
  large traces, consider using `StreamingTraceProtoBuilder` to avoid high
-  memory usage.
  """

  def __init__(self):
    """Initializes the TraceProtoBuilder."""
    self.trace = Trace()

  def add_packet(self) -> TracePacket:
    """Adds a packet to the trace and returns the packet for modification."""
    return self.trace.packet.add()

  def serialize(self) -> bytes:
    """Serializes the entire trace into a byte string."""
    return self.trace.SerializeToString()


class StreamingTraceProtoBuilder:
  """A streaming builder for creating Perfetto traces into a file.

  This class is designed for generating large Perfetto traces without holding
  the entire trace in memory. It writes each `TracePacket` to a file-like
  object as it is created, making it memory-efficient.

  The API is slightly different from `TraceProtoBuilder`. Instead of adding a
  packet to an internal list, you first create a packet, populate it, and then
  explicitly write it to the stream.

  Example:
    with open('my_streamed_trace.pftrace', 'wb') as f:
      builder = StreamingTraceProtoBuilder(f)

      # Create and write the first packet
      packet1 = builder.create_packet()
      packet1.timestamp = 1000
      packet1.track_event.name = "My Event"
      builder.write_packet(packet1)

      # Create and write the second packet
      packet2 = builder.create_packet()
      packet2.timestamp = 2000
      packet2.track_event.name = "Another Event"
      builder.write_packet(packet2)
  """

  def __init__(self, file: IO[bytes]):
    """Initializes the StreamingTraceProtoBuilder with a file-like object.

    Args:
      file: A file-like object opened in binary write mode (e.g., the result
            of `open('trace.pftrace', 'wb')`).
    """
    self._file = file

  def create_packet(self) -> TracePacket:
    """Creates a new, empty TracePacket object.

    This packet is not yet part of the trace. After populating its fields,
    you must call `write_packet()` to add it to the output stream.

    Returns:
      A new `TracePacket` instance.
    """
    return TracePacket()

  def write_packet(self, packet: TracePacket):
    """Serializes and writes a TracePacket to the file stream.

    The packet is wrapped inside a Trace proto as is expected by the Perfetto
    analysis and visualization tooling.

    Args:
      packet: The `TracePacket` to be written to the file.
    """
    trace = Trace()
    trace.packet.append(packet)
    self._file.write(trace.SerializeToString())
