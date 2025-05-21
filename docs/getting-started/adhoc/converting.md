# Converting arbitrary timestamped data to Perfetto

If you have existing logs or timestamped data from your own systems, you don't
need to miss out on Perfetto's powerful visualization and analysis capabilities.
By converting your data into Perfetto's native protobuf-based `TrackEvent`
format, you can create synthetic traces that can be opened in the Perfetto UI
and queried with Trace Processor.

This page provides a guide on how to programmatically generate these synthetic
traces. While the underlying format is protobuf, the examples here will use
Python with a helper class to demonstrate the concepts.

## The Basics: Trace and TracePacket

A Perfetto trace file (`.pftrace` or `.perfetto-trace`) is essentially a
sequence of
[TracePacket](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/trace_packet.proto)
messages, wrapped in a root
[Trace](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/trace.proto)
message.

For generating traces from custom data, the most important `TracePacket` payload
is the
[TrackEvent](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/track_event/track_event.proto).
`TrackEvent` allows you to define:

- **Tracks**: Timelines for processes, threads, or custom concepts.
- **Slices**: Events with a name, start timestamp, and duration (e.g., function
  calls, tasks).
- **Counters**: Numeric values that change over time (e.g., memory usage, custom
  metrics).
- **Flows**: Arrows connecting related slices across different tracks.

## Using the Python TraceProtoBuilder

For the examples below, we'll use a Python class `TraceProtoBuilder` to simplify
the creation of the trace.

```python
# Make sure you have the Perfetto protobufs installed or available in your PYTHONPATH
# e.g., pip install perfetto
#
# Save this as trace_proto_builder.py or similar
from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace
from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import TracePacket
# Import other necessary protos from the perfetto.protos.perfetto.trace.track_event package
# e.g.:
# from perfetto.protos.perfetto.trace.track_event.track_event_pb2 import TrackEvent
# from perfetto.protos.perfetto.trace.track_event.track_descriptor_pb2 import TrackDescriptor
# from perfetto.protos.perfetto.trace.track_event.process_descriptor_pb2 import ProcessDescriptor
# from perfetto.protos.perfetto.trace.track_event.thread_descriptor_pb2 import ThreadDescriptor


class TraceProtoBuilder:
  """A builder for creating Perfetto traces from Python."""

  def __init__(self):
    """Initializes the TraceProtoBuilder."""
    self.trace = Trace()

  def add_packet(self) -> TracePacket:
    """Adds a packet to the trace and returns the packet for modification."""
    return self.trace.packet.add()

  def serialize(self) -> bytes:
    """Serializes the trace to bytes."""
    return self.trace.SerializeToString()

  def write_to_file(self, filename: str):
    """Serializes and writes the trace to a file."""
    with open(filename, 'wb') as f:
      f.write(self.serialize())

# Example usage (will be shown in detail in subsequent sections):
# builder = TraceProtoBuilder()
# packet = builder.add_packet()
# packet.timestamp = 100
# # ... configure packet ...
# builder.write_to_file("my_custom_trace.pftrace")
```
