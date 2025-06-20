# Advanced Guide to Programmatic Trace Generation

This page serves as an advanced reference for programmatically creating Perfetto
trace files. It builds upon the foundational concepts and examples presented in
"[Converting arbitrary timestamped data to Perfetto](/docs/getting-started/converting.md)".

We assume you are familiar with:

- The basic structure of Perfetto traces (a `Trace` message containing a stream
  of `TracePacket` messages).
- Using the `TrackEvent` payload within `TracePacket` to create custom tracks
  with various types of slices (simple, nested, asynchronous), counters, and
  flows.
- The Python script template (`trace_converter_template.py`) for generating
  traces, and that the Python examples provided here are intended to be used
  within its `populate_packets(builder)` function.

This guide will currently focus on advanced `TrackEvent` features, such as:

- Associating your timeline data with operating system (OS) processes and
  threads for richer integration.
- Explicit track sorting and data interning for optimizing trace size and
  detail.

While `TrackEvent` is a primary method for representing timeline data,
`TracePacket` is a versatile container. In the future, this guide may expand to
cover other `TracePacket` payloads useful for synthetic trace generation.

The examples will continue to use Python, but the principles apply to any
language with Protocol Buffer support. For complete definitions of all available
fields, always refer to the official Perfetto protobuf sources, particularly
[TracePacket](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/trace_packet.proto)
and its various sub-messages, including
[TrackEvent](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace/track_event/track_event.proto).

## Associating Tracks with Operating System Concepts

While the
"[Converting arbitrary timestamped data to Perfetto](/docs/getting-started/converting.md)"
guide demonstrated creating generic custom tracks, you can provide more specific
context to Perfetto by associating your tracks with operating system (OS)
processes and threads. This allows Perfetto's UI and analysis tools to offer
richer integration and better correlation with other system-wide data.

### Associating Tracks with Processes

You can create a top-level track that represents an OS process. Any other custom
tracks (which might contain slices or counters) can then be parented to this
process track. This helps in:

- **UI Grouping:** Your custom tracks will appear under the specified process
  name and PID in the Perfetto UI, alongside any other data collected for that
  process (e.g., CPU scheduling, memory counters).
- **Correlation:** Events on your custom tracks can be more easily correlated
  with system-level activity related to that process.
- **Clear Identification:** Explicitly naming the process and providing its PID
  makes it unambiguous which process your custom data pertains to.

To define a process track, you populate the `process` field within its
`TrackDescriptor`. At a minimum, you should provide a `pid` and ideally a
`process_name`.

#### Python Example

Let's say you want to emit a custom counter (e.g. "Active DB Connections") and
have it appear under a specific process named "MyDatabaseService" with PID 1234.

Copy the following Python code into the `populate_packets(builder)` function in
your `trace_converter_template.py` script.

<details>
<summary><a style="cursor: pointer;"><b>Click to expand/collapse Python code</b></a></summary>

```python
    TRUSTED_PACKET_SEQUENCE_ID = 8008

    # --- Define OS Process ---
    PROCESS_ID = 1234
    PROCESS_NAME = "MyDatabaseService"

    # Define a UUID for the process track
    process_track_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    # 1. Define the Process Track
    # This packet establishes "MyDatabaseService (1234)" in the trace.
    packet = builder.add_packet()
    desc = packet.track_descriptor
    desc.uuid = process_track_uuid
    desc.process.pid = PROCESS_ID
    desc.process.process_name = PROCESS_NAME
    # This track itself usually doesn't have events, it serves as a parent.

    # --- Define a Custom Counter Track parented to the Process ---
    db_connections_counter_track_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    packet = builder.add_packet()
    desc = packet.track_descriptor
    desc.uuid = db_connections_counter_track_uuid
    desc.parent_uuid = process_track_uuid # Link to the process track
    desc.name = "Active DB Connections"
    # Mark this track as a counter track
    desc.counter.unit_name = "connections" # Optional: specify units

    # Helper to add a counter event
    def add_counter_event(ts, value, counter_track_uuid):
        packet = builder.add_packet()
        packet.timestamp = ts
        packet.track_event.type = TrackEvent.TYPE_COUNTER
        packet.track_event.track_uuid = counter_track_uuid
        packet.track_event.counter_value = float(value)
        packet.trusted_packet_sequence_id = TRUSTED_PACKET_SEQUENCE_ID

    # 3. Emit counter values on the custom counter track
    add_counter_event(ts=10000, value=5, counter_track_uuid=db_connections_counter_track_uuid)
    add_counter_event(ts=10100, value=7, counter_track_uuid=db_connections_counter_track_uuid)
    add_counter_event(ts=10200, value=6, counter_track_uuid=db_connections_counter_track_uuid)
```

</details>

TODO: this looks like so.

Once you have defined a process track, you can parent various other kinds of
tracks to it. This includes tracks for specific threads within that process (see
next section), as well as custom tracks for process-wide counters (as shown
above) or groups of asynchronous operations related to this process (using the
techniques for asynchronous slices described in the
"[Converting arbitrary timestamped data to Perfetto](/docs/getting-started/converting.md)"
guide).

### Associating Tracks with Threads

You can create tracks that are explicitly associated with specific threads
within an OS process. This is the most common way to represent thread-specific
activity, such as function call stacks or thread-local counters.

**Benefits:**

- **Correct UI Placement:** When a thread track's `pid` and `tid` are specified
  in its `TrackDescriptor`, the Perfetto UI typically groups it under the
  corresponding process (identified by that `pid`). This helps organize the
  trace.
- **Correlation with System Data:** Perfetto can automatically correlate events
  on your thread track with system-level data for that thread, such as CPU
  scheduling slices.
- **Clear Naming:** You can provide a human-readable name for your thread.

To define a thread track:

1.  Create a `TrackDescriptor` for the thread.
2.  Populate its `thread` field, providing the `pid` of the process this thread
    belongs to and the unique `tid` of the thread. You should also set
    `thread_name`.
3.  Optionally and encouraged, you can also define a separate `TrackDescriptor`
    for the parent process itself (using its `process` field and `pid`), though
    it's not strictly required for the thread track to be recognized _as a
    thread of that PID_. The UI often infers process groupings from PIDs present
    in thread tracks.

**Python Example: Thread-Specific Slices**

This example defines a thread "MainWorkLoop" (TID 5678) belonging to process
"MyApplication" (PID 1234). It then emits a couple of slices directly onto this
thread's track. We also define a track for the process itself for clarity,
though the thread track's association is primarily through its `pid` and `tid`
fields.

Copy the following Python code into the `populate_packets(builder)` function in
your `trace_converter_template.py` script.

<details>
<summary><a style="cursor: pointer;"><b>Click to expand/collapse Python code</b></a></summary>

```python
    TRUSTED_PACKET_SEQUENCE_ID = 8009

    # --- Define OS Process and Thread IDs and Names ---
    APP_PROCESS_ID = 1234
    APP_PROCESS_NAME = "MyApplication"
    MAIN_THREAD_ID = 5678
    MAIN_THREAD_NAME = "MainWorkLoop"

    # --- Define UUIDs for the tracks ---
    # While not strictly necessary to parent a thread track to a process track
    # for the UI to group them by PID, defining a process track can be good practice
    # if you want to name the process explicitly or attach process-scoped tracks later.
    app_process_track_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    main_thread_track_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    # 1. Define the Process Track (Optional, but good for naming the process)
    packet = builder.add_packet()
    desc = packet.track_descriptor
    desc.uuid = app_process_track_uuid
    desc.process.pid = APP_PROCESS_ID
    desc.process.process_name = APP_PROCESS_NAME

    # 2. Define the Thread Track
    # The .thread.pid field associates it with the process.
    # No parent_uuid is set here; UI will group by PID.
    packet = builder.add_packet()
    desc = packet.track_descriptor
    desc.uuid = main_thread_track_uuid
    # desc.parent_uuid = app_process_track_uuid # This line is NOT used
    desc.thread.pid = APP_PROCESS_ID
    desc.thread.tid = MAIN_THREAD_ID
    desc.thread.thread_name = MAIN_THREAD_NAME

    # Helper to add a slice event to a specific track
    def add_slice_event(ts, event_type, event_track_uuid, name=None):
        packet = builder.add_packet()
        packet.timestamp = ts
        packet.track_event.type = event_type
        packet.track_event.track_uuid = event_track_uuid
        if name:
            packet.track_event.name = name
        packet.trusted_packet_sequence_id = TRUSTED_PACKET_SEQUENCE_ID

    # 3. Emit slices on the main_thread_track_uuid
    add_slice_event(ts=15000, event_type=TrackEvent.TYPE_SLICE_BEGIN,
                    event_track_uuid=main_thread_track_uuid, name="ProcessInputEvent")
    # Nested slice
    add_slice_event(ts=15050, event_type=TrackEvent.TYPE_SLICE_BEGIN,
                    event_track_uuid=main_thread_track_uuid, name="UpdateState")
    add_slice_event(ts=15150, event_type=TrackEvent.TYPE_SLICE_END, # Ends UpdateState
                    event_track_uuid=main_thread_track_uuid)
    add_slice_event(ts=15200, event_type=TrackEvent.TYPE_SLICE_END, # Ends ProcessInputEvent
                    event_track_uuid=main_thread_track_uuid)

    add_slice_event(ts=16000, event_type=TrackEvent.TYPE_SLICE_BEGIN,
                    event_track_uuid=main_thread_track_uuid, name="RenderFrame")
    add_slice_event(ts=16500, event_type=TrackEvent.TYPE_SLICE_END,
                    event_track_uuid=main_thread_track_uuid)
```

</details>

TODO: this looks like so.

## Advanced Track Customization

Beyond associating tracks with OS concepts, Perfetto offers ways to fine-tune
how your tracks are presented and how data is encoded.

### Controlling Track Sorting Order

By default, the Perfetto UI applies its own heuristics to sort tracks (e.g.,
alphabetically by name, or by track UUID). However, for complex custom traces,
you might want to explicitly define the order in which sibling tracks appear
under a parent. This is achieved using the `child_ordering` field on the parent
`TrackDescriptor` and, for `EXPLICIT` ordering, the `sibling_order_rank` on the
child `TrackDescriptor`s.

This `child_ordering` setting on a parent track only affects its direct
children.

Available `child_ordering` modes (defined in
`TrackDescriptor.ChildTracksOrdering`):

- `ORDERING_UNSPECIFIED`: The default. The UI will use its own heuristics.
- `LEXICOGRAPHIC`: Child tracks are sorted alphabetically by their `name`.
- `CHRONOLOGICAL`: Child tracks are sorted based on the timestamp of the
  earliest `TrackEvent` that occurs on each of them. Tracks with earlier events
  appear first.
- `EXPLICIT`: Child tracks are sorted based on the `sibling_order_rank` field
  set in their respective `TrackDescriptor`s. Lower ranks appear first. If ranks
  are equal, or if `sibling_order_rank` is not set, the tie-breaking order is
  undefined.

**Note:** The UI treats these as strong hints. While it generally respects these
orderings, there are contexts in which the UI reserves the right _not_ to show
them in this order; generally this would be if the user explicitly requested
this or if the UI has some special handling for these tracks.

**Python Example: Demonstrating All Sorting Types**

This example defines three parent tracks, each demonstrating a different
`child_ordering` mode.

Copy the following Python code into the `populate_packets(builder)` function in
your `trace_converter_template.py` script.

<details>
<summary><a style="cursor: pointer;"><b>Click to expand/collapse Python code</b></a></summary>

```python
    TRUSTED_PACKET_SEQUENCE_ID = 9000

    # Helper to define a TrackDescriptor
    def define_custom_track(track_uuid, name, parent_track_uuid=None, child_ordering_mode=None, order_rank=None):
        packet = builder.add_packet()
        desc = packet.track_descriptor
        desc.uuid = track_uuid
        desc.name = name
        if parent_track_uuid:
            desc.parent_uuid = parent_track_uuid
        if child_ordering_mode:
            desc.child_ordering = child_ordering_mode
        if order_rank is not None:
            desc.sibling_order_rank = order_rank

    # Helper to add a simple instant event
    def add_instant_event(ts, track_uuid, event_name):
        packet = builder.add_packet()
        packet.timestamp = ts
        packet.track_event.type = TrackEvent.TYPE_INSTANT
        packet.track_event.track_uuid = track_uuid
        packet.track_event.name = event_name
        packet.trusted_packet_sequence_id = TRUSTED_PACKET_SEQUENCE_ID

    # --- 1. Lexicographical Sorting Example ---
    parent_lex_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    define_custom_track(parent_lex_uuid, "Lexicographic Parent",
                        child_ordering_mode=TrackDescriptor.ChildTracksOrdering.LEXICOGRAPHIC)

    child_c_lex_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    child_a_lex_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    child_b_lex_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    define_custom_track(child_c_lex_uuid, "C-Item (Lex)", parent_track_uuid=parent_lex_uuid)
    define_custom_track(child_a_lex_uuid, "A-Item (Lex)", parent_track_uuid=parent_lex_uuid)
    define_custom_track(child_b_lex_uuid, "B-Item (Lex)", parent_track_uuid=parent_lex_uuid)

    add_instant_event(ts=100, track_uuid=child_c_lex_uuid, event_name="Event C")
    add_instant_event(ts=100, track_uuid=child_a_lex_uuid, event_name="Event A")
    add_instant_event(ts=100, track_uuid=child_b_lex_uuid, event_name="Event B")
    # Expected UI order under "Lexicographic Parent": A-Item, B-Item, C-Item

    # --- 2. Chronological Sorting Example ---
    parent_chrono_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    define_custom_track(parent_chrono_uuid, "Chronological Parent",
                        child_ordering_mode=TrackDescriptor.ChildTracksOrdering.CHRONOLOGICAL)

    child_late_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    child_early_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    child_middle_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    define_custom_track(child_late_uuid, "Late Event Track", parent_track_uuid=parent_chrono_uuid)
    define_custom_track(child_early_uuid, "Early Event Track", parent_track_uuid=parent_chrono_uuid)
    define_custom_track(child_middle_uuid, "Middle Event Track", parent_track_uuid=parent_chrono_uuid)

    add_instant_event(ts=2000, track_uuid=child_late_uuid, event_name="Late Event")
    add_instant_event(ts=1000, track_uuid=child_early_uuid, event_name="Early Event")
    add_instant_event(ts=1500, track_uuid=child_middle_uuid, event_name="Middle Event")
    # Expected UI order under "Chronological Parent": Early, Middle, Late Event Track

    # --- 3. Explicit Sorting Example ---
    parent_explicit_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    define_custom_track(parent_explicit_uuid, "Explicit Parent",
                        child_ordering_mode=TrackDescriptor.ChildTracksOrdering.EXPLICIT)

    child_rank10_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    child_rank_neg5_uuid = uuid.uuid4().int & ((1 << 63) - 1)
    child_rank0_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    define_custom_track(child_rank10_uuid, "Explicit Rank 10",
                        parent_track_uuid=parent_explicit_uuid, order_rank=10)
    define_custom_track(child_rank_neg5_uuid, "Explicit Rank -5",
                        parent_track_uuid=parent_explicit_uuid, order_rank=-5)
    define_custom_track(child_rank0_uuid, "Explicit Rank 0",
                        parent_track_uuid=parent_explicit_uuid, order_rank=0)

    add_instant_event(ts=3000, track_uuid=child_rank10_uuid, event_name="Event Rank 10")
    add_instant_event(ts=3000, track_uuid=child_rank_neg5_uuid, event_name="Event Rank -5")
    add_instant_event(ts=3000, track_uuid=child_rank0_uuid, event_name="Event Rank 0")
    # Expected UI order under "Explicit Parent": Rank -5, Rank 0, Rank 10
```

</details>

TODO: this looks like so.

### Interning Data for Trace Size Optimization

Interning is a technique used to reduce the size of trace files by emitting
frequently repeated strings (like event names or categories) only once in the
trace. Subsequent references to these strings use a compact integer identifier
(an "interning ID" or `iid`). This is particularly useful when you have many
events that share the same name or other string-based attributes.

**How it works:**

1.  **Define Interned Data:** In a `TracePacket`, you include an `interned_data`
    message. Inside this, you map your strings to `iid`s. For example, you can
    define `event_names` where each entry has an `iid` (a non-zero integer you
    choose) and a `name` string. This packet _establishes_ the mapping.
2.  **Reference by IID:** In subsequent `TrackEvent`s (within the same
    `trusted_packet_sequence_id` and before the interned state is cleared),
    instead of setting the `name` field directly, you set the corresponding
    `name_iid` field to the integer `iid` you defined.
3.  **Sequence Flags:** The `TracePacket.sequence_flags` field is crucial:

    - `SEQ_INCREMENTAL_STATE_CLEARED` (value 1): Set this on a packet if the
      interning dictionary (and other incremental state) for this sequence
      should be considered reset _before_ processing this packet's
      `interned_data`. This is often used on the first packet of a sequence that
      defines interned entries.
    - `SEQ_NEEDS_INCREMENTAL_STATE` (value 2): Set this on any packet that
      _either defines new interned data entries OR uses iids_ that were defined
      in previous packets (within the current valid state of the sequence).

    A typical packet that _initializes_ the interning dictionary for a sequence
    will set both flags:
    `TracePacket.SEQ_INCREMENTAL_STATE_CLEARED | TracePacket.SEQ_NEEDS_INCREMENTAL_STATE`.
    Packets that _use_ these established interned entries (or add more entries
    to the existing valid dictionary) will set
    `TracePacket.SEQ_NEEDS_INCREMENTAL_STATE`.

**Python Example: Interning Event Names**

This example shows how to define an interned string for an event name and then
use it multiple times.

Copy the following Python code into the `populate_packets(builder)` function in
your `trace_converter_template.py` script.

<details>
<summary><a style="cursor: pointer;"><b>Click to expand/collapse Python code</b></a></summary>

```python
    TRUSTED_PACKET_SEQUENCE_ID = 9002

    # --- Define Track UUID ---
    interning_track_uuid = uuid.uuid4().int & ((1 << 63) - 1)

    # Helper to define a TrackDescriptor
    def define_custom_track(track_uuid, name):
        packet = builder.add_packet()
        desc = packet.track_descriptor
        desc.uuid = track_uuid
        desc.name = name

    # 1. Define the track
    define_custom_track(interning_track_uuid, "Interning Demo Track")

    # --- Define Interned Event Name ---
    INTERNED_EVENT_NAME_IID = 1 # Choose a unique iid (non-zero)
    VERY_LONG_EVENT_NAME = "MyFrequentlyRepeatedLongEventNameThatTakesUpSpace"

    # Helper to add a TrackEvent packet, managing interning and sequence flags
    def add_slice_with_interning(ts, event_type, name_iid=None, name_literal=None, define_new_internment=False, new_intern_iid=None, new_intern_name=None):
        packet = builder.add_packet()
        packet.timestamp = ts
        tev = packet.track_event
        tev.type = event_type
        tev.track_uuid = interning_track_uuid

        if name_iid:
            tev.name_iid = name_iid
        elif name_literal and event_type != TrackEvent.TYPE_SLICE_END:
            tev.name = name_literal

        if define_new_internment:
            # This packet defines new interned data.
            # We'll also clear any prior state for this sequence.
            if new_intern_iid and new_intern_name:
                entry = packet.interned_data.event_names.add()
                entry.iid = new_intern_iid
                entry.name = new_intern_name
            packet.sequence_flags = TracePacket.SEQ_INCREMENTAL_STATE_CLEARED | TracePacket.SEQ_NEEDS_INCREMENTAL_STATE
        else:
            # This packet uses existing interned data (or has no interned fields)
            # but is part of a sequence that relies on incremental state.
            packet.sequence_flags = TracePacket.SEQ_NEEDS_INCREMENTAL_STATE

        packet.trusted_packet_sequence_id = TRUSTED_PACKET_SEQUENCE_ID
        return packet

    # --- Packet 1: Define the interned name and start a slice using it ---
    add_slice_with_interning(
        ts=1000,
        event_type=TrackEvent.TYPE_SLICE_BEGIN,
        name_iid=INTERNED_EVENT_NAME_IID,
        define_new_internment=True, # This packet defines/resets internment
        new_intern_iid=INTERNED_EVENT_NAME_IID,
        new_intern_name=VERY_LONG_EVENT_NAME
    )

    # End the first slice
    add_slice_with_interning(
        ts=1100,
        event_type=TrackEvent.TYPE_SLICE_END
        # No name_iid needed for END, uses existing interned state context
    )

    # --- Packet 2: Use the Interned Event Name Again ---
    add_slice_with_interning(
        ts=1200,
        event_type=TrackEvent.TYPE_SLICE_BEGIN,
        name_iid=INTERNED_EVENT_NAME_IID # Re-use the iid
        # define_new_internment is False by default, so this uses existing state
    )

    # End the second slice
    add_slice_with_interning(
        ts=1300,
        event_type=TrackEvent.TYPE_SLICE_END
    )
```

</details>

TODO: this looks like so.
