# Writing TrackEvent Protos Synthetically
This page acts as a reference guide to synthetically generate TrackEvent,
Perfetto's native protobuf based tracing format. This allows using Perfetto's
analysis and visualzation without using collecting traces using the Perfetto
SDK.

TrackEvent protos can be manually written using the
[official protobuf library](https://protobuf.dev/reference/) or any other
protobuf-compatible library. To be language-agnostic, the rest of this page
will show examples using the
[text format](https://protobuf.dev/reference/protobuf/textformat-spec/)
representation of protobufs.

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
packet: {
  track_descriptor: {
    uuid: 894893984                     # 64-bit random number.
    process: {
      pid: 1234                         # PID for your process.
      process_name: "My process name"
    }
  }
}

# Emit this packet once *before* you emit the first event for this thread.
packet: {
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
packet: {
  timestamp: 200
  track_event: {
    type: TYPE_SLICE_BEGIN
    track_uuid: 49083589894             # Same random number from above.
    name: "My special parent"
  }
  trusted_packet_sequence_id: 3903809   # Generate *once*, use throughout.
}
packet: {
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
packet: {
  timestamp: 290
  track_event: {
    type: TYPE_SLICE_END
    track_uuid: 49083589894
  }
  trusted_packet_sequence_id: 3903809
}
packet: {
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
multiple threads of a process. A process-scoped slice can start on a thread
A and end on a thread B. Examples include work submitted to thread pools
and coroutines.

Process tracks can be named corresponding to the executor and can also have
child slices in an identical way to thread-scoped slices. Importantly, this
means slices on a single track must **strictly nest** inside each other
without overlapping.

As separating each track in the UI can cause a lot of clutter, the UI
visually merges process tracks with the same name in each process. Note that
this **does not** change the data model (e.g. in trace processor
tracks remain separated) as this is simply a visual grouping.

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

## Flows
NOTE: in the legacy JSON tracing format, this section correspond to s/t/f
events.

Flows allow connecting any number of slices with arrows. The semantic meaning
of the arrow varies across different applications but most commonly it is used
to track work passing between threads or processes: e.g. the UI thread asks a
background thread to do some work and notify when the result is available.

NOTE: a single flow *cannot* fork ands imply represents a single stream of
arrows from one slice to the next. See [this](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/perfetto_trace.proto;drc=ba05b783d9c29fe334a02913cf157ea1d415d37c;l=9604) comment for information.

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
