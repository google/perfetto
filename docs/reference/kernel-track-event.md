# Kernel track events: format and conventions

This page describes a convention for structuring Linux kernel tracepoints in a
way that enables perfetto to automatically present them as slice/counter tracks
at the UI and SQL levels, without having to change or rebuild perfetto code.

This is a perfetto convention, and does not have (or need) any dedicated
upstream kernel code. It's best used when hacking on a local kernel, or writing
a self-contained module that won't be upstreamed. It is also not explicitly
tied to static tracepoints, a dynamic probe (e.g. kprobe) that creates a
`tracefs` entry with the relevant fields will also work.

This page is structured as a reference, an introduction with **examples and
screenshots** of resulting UI is at ["Intrumenting the Linux kernel with
ftrace"][ftrace-intro-link].

[ftrace-intro-link]: /docs/getting-started/ftrace#part-c-simple-slice-counter-visualisations-without-modifying-perfetto-code-kernel-track-events-

*This convention is still malleable, if you end up using it and/or finding
issues with the design, please send an email to our mailing list or file a
github issue.*

## Slices and instants

Perfetto looks for fields with specific types and names in the event's data
representation. This is defined by `TP_STRUCT__entry()` when using the
`TRACE_EVENT()` macro to define the tracepoint.

For representing slices (begin + end) and instants, grouped by tracks, the
well-known fields are:

| required? | type | name |
| --- | --- | --- |
| required | char | track\_event\_type |
| required | \_\_string | slice\_name |
| optional | intX | scope\_{...} |
| optional | \_\_string | track\_name |

Where `intX` represents any integral type, and `__string` is the kernel type
used for storing dynamically-sized strings in tracing events.

At runtime, the event payloads will be interpreted as follows:

* `track_event_type`:
  * `'B'` opens a named slice.
  * `'E'` ends the last opened slice within the track.
  * `'I'` sets a named instant (zero duration) event.

* `slice_name`: the name of the slice for begin ('B') and instant ('I') events,
ignored for end events.

* `track_name`: if set, overrides the track's name. The default is the
tracepoint's name.

* `scope_{...}`: if set, specifies the scoping id of the track, which is used
  as a grouping key for the tracks. The field name can have an arbitrary suffix
  that makes sense within your subsystem, but there are also a few well-known
  names that perfetto can use as a hint when presenting the tracks in the UI.
  The id does not have to be related to an OS-level concept.
  * `scope_tgid`: for process-scoped tracks, where the value must be of a valid
    process (though the calling thread does not need to be within that process).
  * `scope_cpu`: for cpu-scoped tracks (emitting code does not need to be
    running on that cpu).
  * `scope_your_feature_idx`: for your own track id assignments.
  * *default*: thread-scoped track (using the thread id of the thread hitting
    the tracepoint, as recorded by the ftrace system itself).

Additionally:

The tracepoint name and the subsystem can be arbitrary. Your headers can
declare an arbitrary amount of tracepoints that match these templates. Each
tracepoint will be processed indepdendently.

There are no constraints on having additional fields, the field order or other
parts of the `TRACE_EVENT()` declaration. Note that this includes the printk
specifier, so the textual formatting of the tracepoint can be arbitrary (you
don't even need to print the perfetto-specific fields).

## Counters

For representing counter values, grouped by tracks, the well-known fields are:

| required? | type | name |
| --- | --- | --- |
| required | intX | counter\_value |
| optional | intX | scope\_{...} |
| optional | \_\_string | track\_name |

## Details on scoping (grouping) events

This section explains the rules of how the recorded events get grouped into
tracks, as generally a trace recorded using a single tracepoint can result in N
separate tracks. The grouping rules are the same for slice and counter tracks.

**NB:** slices on slice tracks *must* have strict nesting - all slices must
terminate before their parents (see the concept of [async
slices][async-slice-link] for more details). You need to use track naming or
scoping to ensure that that invariant is preserved.

The default behaviour (if you only specify the mandatory fields) is
thread-scoped. Events are grouped by the thread id of the thread(s) hitting the
tracepoints. There will be one track per thread with events. The end ('E')
events will terminate the last opened slice on that thread.

If the event has a field prefixed with `scope_`, the events will be grouped by
the value of that field, with some predefined names having special meaning (see
above). For example, if you specify a `scope_tgid`, that turns the track
process-scoped - all events sharing the same `scope_tgid` value will be put on
the same track. Further, the UI will present that track in the process' group.

If your events include the `track_name` field, then events become grouped by
that name as an additional dimension to the above. That is, the end ('E') event
will terminate the last opened slice with that exact track name, even if there
are multiple named tracks within the same thread/process/cpu/etc scope.

The net effect is that recorded events are grouped by the unique combination
of: `{tracepoint} x {track name} x {scope id}`. With the last two defaulting to
the tracepoint name and thread id respectively.

