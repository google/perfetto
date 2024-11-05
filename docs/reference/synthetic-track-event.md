# Writing TrackEvent Protos Synthetically

This page acts as a reference guide to synthetically generate TrackEvent,
Perfetto's native protobuf based tracing format. This allows using Perfetto's
analysis and visualzation without using collecting traces using the Perfetto
SDK.

TrackEvent protos can be manually written using the
[official protobuf library](https://protobuf.dev/reference/) or any other
protobuf-compatible library. To be language-agnostic, the rest of this page will
show examples using the
[text format](https://protobuf.dev/reference/protobuf/textformat-spec/)
representation of protobufs.

The root container of the protobuf-based traces is the
[Trace](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/trace/trace.proto)
message which itself is simply a repeated field of
[TracePacket](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/trace/trace_packet.proto)
messages.

## Thread-scoped (sync) slices

NOTE: in the legacy JSON tracing format, this section correspond to B/E/I/X
events with the associated M (metadata) events.

Thread scoped slices are used to trace execution of functions on a single
thread. As only one function runs on a single thread over time, this requires
that child slices nest perfectly inside parent slices and do not partially
overlap.

![Thread track event in UI](/docs/images/synthetic-track-event-thread.png)

This is corresponds to the following protos:

```
# Emit this packet once *before* you emit the first event for this process.
packet {
  track_descriptor: {
    uuid: 894893984                     # 64-bit random number.
    process: {
      pid: 1234                         # PID for your process.
      process_name: "My process name"
    }
  }
}

# Emit this packet once *before* you emit the first event for this thread.
packet {
  track_descriptor: {
    uuid: 49083589894                   # 64-bit random number.
    parent_uuid: 894893984              # UUID from above.
    thread: {
      pid: 1234                         # PID for your process.
      tid: 5678                         # TID for your thread.
      thread_name: "My thread name"
    }
  }
}

# The events for this thread.
packet {
  timestamp: 200
  track_event: {
    type: TYPE_SLICE_BEGIN
    track_uuid: 49083589894             # Same random number from above.
    name: "My special parent"
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
}
packet {
  timestamp: 250
  track_event: {
    type: TYPE_SLICE_BEGIN
    track_uuid: 49083589894
    name: "My special child"
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 285
  track_event {
    type: TYPE_INSTANT
    track_uuid: 49083589894
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 290
  track_event: {
    type: TYPE_SLICE_END
    track_uuid: 49083589894
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 300
  track_event: {
    type: TYPE_SLICE_END
    track_uuid: 49083589894
  }
  trusted_packet_sequence_id: 3903809
}
```

## Process-scoped (async) slices

NOTE: in the legacy JSON tracing format, this section corresponds to b/e/n
events with the associated M (metadata) events.

Process-scoped slices are useful to trace execution of a "piece of work" across
multiple threads of a process. A process-scoped slice can start on a thread A
and end on a thread B. Examples include work submitted to thread pools and
coroutines.

Process tracks can be named corresponding to the executor and can also have
child slices in an identical way to thread-scoped slices. Importantly, this
means slices on a single track must **strictly nest** inside each other without
overlapping.

As separating each track in the UI can cause a lot of clutter, the UI visually
merges process tracks with the same name in each process. Note that this **does
not** change the data model (e.g. in trace processor tracks remain separated) as
this is simply a visual grouping.

![Process track event in UI](/docs/images/synthetic-track-event-process.png)

This is corresponds to the following protos:

```
# The first track associated with this process.
packet {
  track_descriptor {
    uuid: 48948                         # 64-bit random number.
    name: "My special track"
    process {
      pid: 1234                         # PID for your process
      process_name: "My process name"
    }
  }
}
# The events for the first track.
packet {
  timestamp: 200
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 48948                   # Same random number from above.
    name: "My special parent A"
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
}
packet {
  timestamp: 250
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 48948
    name: "My special child"
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 290
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 48948
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 300
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 48948
  }
  trusted_packet_sequence_id: 3903809
}

# The second track associated with this process. Note how we make the above
# track the "parent" of this track: this means that this track also is
# associated to the same process. Note further this shows as the same visual
# track in the UI but remains separate in the trace and data model. Emitting
# these events on a separate track is necessary because these events overlap
# *without* nesting with the above events.
packet {
  track_descriptor {
      uuid: 2390190934                  # 64-bit random number.
      name: "My special track"
      parent_uuid: 48948
  }
}
# The events for the second track.
packet {
  timestamp: 230
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 2390190934              # Same random number from above.
    name: "My special parent A"
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 260
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 2390190934
    name: "My special child"
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 270
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 2390190934
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 295
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 2390190934
  }
  trusted_packet_sequence_id: 3903809
}
```

## Custom-scoped slices

NOTE: there is no equivalent in the JSON tracing format.

As well as thread-scoped and process-scoped slices, Perfetto supports creating
tracks which are not scoped to any OS-level concept. Moreover, these tracks can
be recursively nested in a tree structure. This is useful to model the timeline
of execution of GPUs, network traffic, IRQs etc.

Note: in the past, modelling such slices may have been done by abusing
processes/threads slices, due to limitations with the data model and the
Perfetto UI. This is no longer necessary and we _strongly_ discourage continued
use of this hack.

![Process track event in UI](/docs/images/synthetic-track-event-custom-tree.png)

This is corresponds to the following protos:

```
packet {
  track_descriptor {
    uuid: 48948                         # 64-bit random number.
    name: "Root"
  }
}
packet {
  track_descriptor {
    uuid: 50001                         # 64-bit random number.
    parent_uuid: 48948                  # UUID of root track.
    name: "Parent B"
  }
}
packet {
  track_descriptor {
    uuid: 50000                         # 64-bit random number.
    parent_uuid: 48948                  # UUID of root track.
    name: "Parent A"
  }
}
packet {
  track_descriptor {
    uuid: 60000                         # 64-bit random number.
    parent_uuid: 50000                  # UUID of Parent A track.
    name: "Child A1"
  }
}
packet {
  track_descriptor {
    uuid: 60001                         # 64-bit random number.
    parent_uuid: 50000                  # UUID of Parent A track.
    name: "Child A2"
  }
}
packet {
  track_descriptor {
    uuid: 70000                         # 64-bit random number.
    parent_uuid: 50001                  # UUID of Parent B track.
    name: "Child B1"
  }
}

# The events for the Child A1 track.
packet {
  timestamp: 200
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 60000                   # Same random number from above.
    name: "A1"
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
}
packet {
  timestamp: 250
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 60000
  }
  trusted_packet_sequence_id: 3903809
}

# The events for the Child A2 track.
packet {
  timestamp: 220
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 60001                   # Same random number from above.
    name: "A2"
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
}
packet {
  timestamp: 240
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 60001
  }
  trusted_packet_sequence_id: 3903809
}

# The events for the Child B1 track.
packet {
  timestamp: 210
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 70000                   # Same random number from above.
    name: "B1"
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
}
packet {
  timestamp: 230
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 70000
  }
  trusted_packet_sequence_id: 3903809
}
```

## Track sorting order

NOTE: the closest equivalent to this in the JSON format is `process_sort_index`
but the Perfetto approach is significantly more flexible.

Perfetto also supports specifying of how the tracks should be visualized in the
UI by default. This is done via the use of the `child_ordering` field which can
be set on `TrackDescriptor`.

For example, to sort the tracks lexicographically (i.e. in alphabetical order):

```
packet {
  track_descriptor {
    uuid: 10
    name: "Root"
    # Any children of the `Root` track will appear in alphabetical order. This
    # does *not* propogate to any indirect descendants, just the direct
    # children.
    child_ordering: LEXICOGRAPHIC
  }
}
# B will appear nested under `Root` but *after* `A` in the UI, even though it
# appears first in the trace and has a smaller UUID.
packet {
  track_descriptor {
    uuid: 11
    parent_uuid: 10
    name: "B"
  }
}
packet {
  track_descriptor {
    uuid: 12
    parent_uuid: 10
    name: "A"
  }
}
```

Chronological order is also supported, this sorts the tracks with the earliest
event first:

```
packet {
  track_descriptor {
    uuid: 10
    name: "Root"
    # Any children of the `Root` track will appear in the order based on the
    # timestamp of the first event on the trace: earlier timestamps will appear
    # higher in the trace. This does *not* propogate to any indirect
    # descendants, just the direct children.
    child_ordering: CHRONOLOGICAL
  }
}

# B will appear before A because B's first slice starts earlier than A's first
# slice.
packet {
  track_descriptor {
    uuid: 11
    parent_uuid: 10
    name: "A"
  }
}
packet {
  timestamp: 220
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 11
    name: "A1"
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 230
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 60000
  }
  trusted_packet_sequence_id: 3903809
}

packet {
  track_descriptor {
    uuid: 12
    parent_uuid: 10
    name: "B"
  }
}
packet {
  timestamp: 210
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 12
    name: "B1"
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 240
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 12
  }
  trusted_packet_sequence_id: 3903809
}
```

Finally, for exact control, you can use the `EXPLICIT` ordering and specify
`sibling_order_rank` on each child track:

```
packet {
  track_descriptor {
    uuid: 10
    name: "Root"
    # Any children of the `Root` track will appear in order specified by
    # `sibling_order_rank` exactly: any unspecified rank is treated as 0
    # implicitly.
    child_ordering: EXPLICIT
  }
}
# C will appear first, then B then A following the order specified by
# `sibling_order_rank`.
packet {
  track_descriptor {
    uuid: 11
    parent_uuid: 10
    name: "B"
    sibling_order_rank: 1
  }
}
packet {
  track_descriptor {
    uuid: 12
    parent_uuid: 10
    name: "A"
    sibling_order_rank: 100
  }
}
packet {
  track_descriptor {
    uuid: 13
    parent_uuid: 10
    name: "C"
    sibling_order_rank: -100
  }
}
```

NOTE: using `EXPLICIT` is strongly discouraged where there is another option.
Other orders are significantly more efficient and also allows for trace
processor and the UI to better understand what you want to do with those tracks.
Moreover, it gives the flexibility for having custom visualization (e.g. Gannt
charts for CHRONOLOGICAL view) based on the type specified.

Further documentation about the sorting order is available on the protos for
[TrackDescriptor](/docs/reference/trace-packet-proto.autogen#TrackDescriptor)
and
[ChildTracksOrdering](/docs/reference/trace-packet-proto.autogen#TrackDescriptor.ChildTracksOrdering).

NOTE: the order specified in the trace is a treated as a hint in the UI not a
gurantee. The UI reserves the right to change the ordering as it sees fit.

## Flows

NOTE: in the legacy JSON tracing format, this section correspond to s/t/f
events.

Flows allow connecting any number of slices with arrows. The semantic meaning of
the arrow varies across different applications but most commonly it is used to
track work passing between threads or processes: e.g. the UI thread asks a
background thread to do some work and notify when the result is available.

NOTE: a single flow _cannot_ fork ands imply represents a single stream of
arrows from one slice to the next. See
[this](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/perfetto_trace.proto;drc=ba05b783d9c29fe334a02913cf157ea1d415d37c;l=9604)
comment for information.

![TrackEvent flows in UI](/docs/images/synthetic-track-event-flow.png)

```
# The main thread of the process.
packet {
  track_descriptor {
    uuid: 93094
    thread {
        pid: 100
        tid: 100
        thread_name: "Main thread"
    }
  }
}
packet {
  timestamp: 200
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 93094
    name: "Request generation"
    flow_ids: 1055895987                  # Random number used to track work
                                          # across threads/processes.
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 300
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 93094
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 400
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 93094
    name: "Process background result"
    flow_ids: 1055895987                  # Same as above.
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 500
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 93094
  }
  trusted_packet_sequence_id: 3903809
}

# The background thread of the process.
packet {
  track_descriptor {
    uuid: 40489498
    thread {
      pid: 100
      tid: 101
      thread_name: "Background thread"
    }
  }
}
packet {
  timestamp: 310
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 40489498
    name: "Background work"
    flow_ids: 1055895987                  # Same as above.
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 385
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 40489498
  }
  trusted_packet_sequence_id: 3903809
}
```

## Counters

NOTE: in the legacy JSON tracing format, this section correspond to C events.

Counters are useful to represent continuous values which change with time.
Common examples include CPU frequency, memory usage, battery charge etc.

![TrackEvent counter in UI](/docs/images/synthetic-track-event-counter.png)

This corresponds to the following protos:

```
# Counter track scoped to a process.
packet {
  track_descriptor {
    uuid: 1388
    process {
      pid: 1024
      process_name: "MySpecialProcess"
    }
  }
}
packet {
  track_descriptor {
    uuid: 4489498
    parent_uuid: 1388
    name: "My special counter"
    counter {}
  }
}
packet {
  timestamp: 200
  track_event {
    type: TYPE_COUNTER
    track_uuid: 4489498
    counter_value: 34567    # Value at start
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 250
  track_event {
    type: TYPE_COUNTER
    track_uuid: 4489498
    counter_value: 67890    # Value goes up
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 300
  track_event {
    type: TYPE_COUNTER
    track_uuid: 4489498
    counter_value: 12345   # Value goes down
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 400
  track_event {
    type: TYPE_COUNTER
    track_uuid: 4489498
    counter_value: 12345   # Final value
  }
  trusted_packet_sequence_id: 3903809
}
```

## Interning

NOTE: there is no equivalent to interning in the JSON tracing format.

Interning is an advanced but powerful feature of the protobuf tracing format
which allows allows for reducing the number of times long strings are emitted in
the trace.

Specifically, certain fields in the protobuf format allow associating an "iid"
(interned id) to a string and using the iid to reference the string in all
future packets. The most commonly used cases are slice names and category names

Here is an example of a trace which makes use of interning to reduce the number
of times a very long slice name is emitted:
![TrackEvent interning](/docs/images/synthetic-track-event-interned.png)

This corresponds to the following protos:

```
packet {
  track_descriptor {
    uuid: 48948                         # 64-bit random number.
    name: "My special track"
    process {
      pid: 1234                         # PID for your process
      process_name: "My process name"
    }
  }
}
packet {
  timestamp: 200
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 48948                   # Same random number from above.
    name_iid: 1                         # References the string in interned_data
                                        # (see below)
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.

  interned_data {
    # Creates a mapping from the iid "1" to the string name: any |name_iid| field
    # in this packet onwards will transparently be remapped to this string by trace
    # processor.
    # Note: iid 0 is *not* a valid IID and should not be used.
    event_names {
      iid: 1
      name: "A very very very long slice name which we don't want to repeat"
    }
  }

  first_packet_on_sequence: true        # Indicates to trace processor that
                                        # this is the first packet on the
                                        # sequence.
  previous_packet_dropped: true         # Same as |first_packet_on_sequence|.

  # Indicates to trace processor that this sequence resets the incremental state but
  # also depends on incrtemental state state.
  # 3 = SEQ_INCREMENTAL_STATE_CLEARED | SEQ_NEEDS_INCREMENTAL_STATE
  sequence_flags: 3
}
packet {
  timestamp: 201
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 48948
  }
  trusted_packet_sequence_id: 3903809
}
packet {
  timestamp: 202
  track_event {
    type: TYPE_SLICE_BEGIN
    track_uuid: 48948                   # Same random number from above.
    name_iid: 1                         # References the string in interned_data
                                        # above.
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
  # 2 = SEQ_NEEDS_INCREMENTAL_STATE
  sequence_flags: 2
}
packet {
  timestamp: 203
  track_event {
    type: TYPE_SLICE_END
    track_uuid: 48948
  }
  trusted_packet_sequence_id: 3903809
}
```
