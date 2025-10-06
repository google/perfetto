# Supporting CloneSession() for write_into_file sessions

**Authors:** @primiano @KirillTim

**Status:** Implemented

**Link to PR:** https://github.com/google/perfetto/pull/2871

## Problem

Until now we never properly supported cloning a write_into_file (aka long-trace)
session. Attempts to do so lead to _oddly defined behavior (One of these
behaviors, however is load bearing and must be preserved).

The rationale for deciding to support this is supporting tracing across reboots
(internal doc [go/perfetto-reboot](http://go/perfetto-reboot)).

### Rationale

* go/perfetto-reboot will require write_into_file=true for the configs we want
  to persist and recover across reboots.
* Internally will apply this to @simonmacm's long power traces
* We still want those traces to be appended to the bugreport.zip
* perfetto --save-all-into-bugreport uses CloneSession() under the hoods.
* Hence we want CloneSession to work with write_into_file traces.

### Current behavior

The current behavior of CloneSession + write_into_file, which happens to be
by "implementation accident", is the following:

* Treat the cloned session as if it was NOT write_into_file
* Snapshot its buffers as usual.
* Reset the read iterator (pretend no data was read from the buffer).
* Issue a readback from IPC from the perfetto_cmd side.

This means that if the session was a proper long-trace with O(second)
file_write_period_ms, CloneSession would ignore what written in the file and
capture what was in the buffer(s).

We believe almost no client relies on this, with one notable exception: Traceur.

Traceur (the app reachable from quick settings tile to capture traces directly
from the device) always uses write_into_file=true. This is because it uses
detached mode, which can only work with write_into_file.

In case of ring buffer traces, it sets the file_write_period_ms to 7 days (the
max allowed), which effectively translates into "write into file ... only at the
end of the trace", effectively leveraging the in-memory buffer as a ring buffer
until then.

The main implication of this is that we MUST respect the file_write_period_ms
and cannot interpret that as an "upper bound hint". We cannot aggressively
force ReadBuffersIntoFile before that time.

### perfetto_cmd caveats

When perfetto_cmd clones a session (`perfetto --clone 42 -o /path/to/trace`),
it doesn't know upfront if the session being cloned is WIF or not. The only
input is the ID of the session being cloned. This is true also for the case of
--save-all-for-bugreport.

## Decision

After various iterations we settled on the following design:

### Tracing Protocol change in consumer_port.proto

1. `CloneSessionRequest` will expect a file descriptor to be attached to every
  request. The FD is optional and will be ignored if the session being cloned
  is not WIF; it is mandatory if the session is WIF. Trying to clone a WIF
  session without passing a FD will fail the CloneSession IPC with an error.

2. `CloneSessionResponse` will return an extra field
  `optional bool was_write_into_file = 5;` to tell perfetto_cmd if the session
  cloned was WIF or not. If true, it will take care of cloning the file (see
  below)

### Tracing service

The TracingService will accept a FD in `CloneSessionArgs`. This FD is mandatory
if the session being cloned is WIF, and is optional (will be disregarded) if
not. Upon CloneSession of a WIF session the tracing service will:

1. Create a copy of the contents of the current tracing file into the FD passed.
2. Issue a tracing protocol flush of the data sources and wait for it.
3. The flush above will cause more data to be written in the tracing buffers
   of the original session.
4. Clone the buffers of the original session.
5. ReadBuffersIntoFile for the new cloned session, effectively appending to the
   file copied in step 1 the data written since the last `file_write_period_ms`
   plus the data recently flushed in step 2.
6. If the consumer tries to ReadBuffers over IPC, it will return only the
   `TracingServiceEvent(read_tracing_buffers_complete=true)` packet and never
   read the trace buffers. This is consistent with what it would happen if a
   consumer was to ReadBuffer via IPC a WIF session regardless of clone.

### perfetto_cmd

perfetto_cmd, when issuing a CloneSession, will always speculatively send the FD
of the output trace file to the service, without knowing if it will be used or
not.

It will find out about WIF=true/false only after the tracing service has cloned
the session and reports back the WIF flag in the newly introduced
`CloneSessionResponse.was_write_into_file`.

If the flag is true, it knows that the service did take care of copying the
trace file and appending the recent data, so it will just quite.

If the flag is false, it will read back the buffer via IPC as usual.

## Alternatives considered

perfetto_cmd could have first queried the service via `QueryServiceState()` to
find out if the session is WIF or not. That would have introduced an extra IPC
roundtrip + extra boilerplate.

Sending the FD to the service _just in case_ and finding out post-facto looks
easier.
