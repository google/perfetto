# TraceBuffer V2 Design document

## Overview

This document covers the design of TraceBufferV2, which is the 2025 rewrite
of the core trace buffer code in occasion of ProtoVM.

TraceBuffer is the non-shared userspace buffer that is used by the tracing
service to hold traced data in memory, until it's either read back or written
into a file. There is one TraceBuffer instance for each `buffers` section of the
[trace config](/docs/concepts/config.md)

## Basic operating principles

NOTE: This section assumes you are familiar with the core concepts exposed in
[Buffers and dataflow](/docs/concepts/buffers.md).

TraceBuffer is a _ring buffer on steroids_. Unfortunately due to the
complications of the protocol (see [Challenges](#key-challenges) section) it is
far from a plain byte-oriented FIFO ring buffer.

Before delving into its complications, let's explore the key operation.

Logically TraceBuffer deals with overlapping streams of data, called
_TraceWriter Sequences_, or in short just _Sequences_:

* A client process that writes trace data acts as a _Producer_. Typically
  1 Producer = 1 Process, but there are cases where a process can host >1
  producers (e.g. if it uses N libraries each statically linking the tracing
  SDK)
* A Producer declares DataSources, which is the unit of enablement/configuration
  in the buffer. DataSources, however, don't make it as an abstraction in the
  buffer. Only TracingService knows about data sources.
* Each data source uses one or more TraceWriter, typically one per thread.
* A TraceWriter writes linear sequences of TracePackets.

From a TraceBuffer viewpoint, the only abstraction visible are the Producer
(identified by a `uint16_t ProducerID`) and TraceWriter (identified by a
`uint16_t WriterID`, within the scoped of a producer). The 32-bit tuple
`{ProducerId, WriterID}` constitutes the unique Sequence ID for TraceBuffer.
Everything in TraceBuffer is keyed by that.

Basic operation:

* Producers commit "chunks" into the SMB (Shared Memory Buffer).
* A Chunk belongs to a `{ProducerID,WriterID}`, has a sequence ID and flags.
* A SMB Chunk contains one or more fragments.
* Typically 1 fragment == 1 packet, with the exception of the first and last
  fragment which MIGHT be continuations of longer fragmented packets. Note that
  a chunk could contain only one fragment that happens to be a continuation
  of a larger packet.
* Chunks are copied almost as-is into the TraceBuffer, +- some metadata
  tracking (more details later).
* At readback time, TraceBuffer reconstructs the sequence of packets and
  reassembles larger fragmented packets.
* Reading is a destructive operation.

Readback gives the following guarrantees:

* TraceBuffer only outputs fully formed packets which are valid
  protobuf-encoded TracePacket messages. Packets that are missing fragments, are
  missing patches or are invalid are dropped.
* Data drops are always tracked and reported through the
  `TracePacket.previous_packet_dropped` flag.
* TraceBuffer tries very hard to avoid _hiding_ valid data: a missing fragment
  or other similar protocol violations should not invalidate the rest of the
  data for the sequence.
* Packets for a sequence are always read back FIFO in the same order of writing.
* TraceBuffer tries hard to also respect FIFO-ness of packets belonging to
  different sequences (this is a new behaviour introduced by TraceBufferV2). So
  data is read back roughly in the same order it has been written (+- pending
  patches and data losses which can cause jumps).

Readback happens in the following cases:

* Read via IPC at the end of the trace: this is what perfetto_cmd does by
  default and is used in most tracing scenarios today. All the contents of the
  buffer are read after tracing stops.
* Periodic reads into file: this happens in
  [long tracing mode][lt]. Every O(seconds)
  (configurable) the buffer is read and the packets extracted are written into
  the file descriptor passed by the consumer.
* Periodics reads over IPC: these are rare. Some third-party tools like
  [GPU Inspector](https://gpuinspector.dev) do that. Architecturally they are
  no different from the case of read into file. TraceBuffer isn't aware of any
  difference between "reading into a file" or "reading via IPC". Those concepts
  exist only in TracingServiceImpl.

Code-wise there are four main entry-points:

Writer-side (Producer-side):

* `CopyChunkUntrusted()`: called when a CommitData IPC is received, or when the
  service performs SMB Scraping (more below)
* `TryPatchChunkContents()`: still part of CommitData IPC.

Reader-side:

* `BeginRead()`: called at readback time at the beginning of each read batch.
* `ReadNextTracePacket()`: called once for each packet until either there are
  no more packets in the buffer or TracingServiceImpl decided it has read
  enough data for the current task (to avoid saturating the IPC channel).

## Key challenges

### RING_BUFFER vs DISCARD

TraceBuffer can operate in two modes.

#### RING_BUFFER

This is the mode used by the majority of the traces. It is also the one with
the most complications. This document focuses on the operation in RING_BUFFER
mode, unless otherwise specified.
This mode can be used for pure ring buffer tracing, or can be coupled with
`write_into_file` to have [long traces][lt]
streamed into disk. In the  which case the
ring buffer serves mainly to decouple the SMB and the I/O activity, to handle
fragment reassembly).

[lt]: /docs/concepts/config.md#long-traces

#### DISCARD

This mode is used for one-shot traces where the user cares about the left-most
part of the trace. This is conceptually easier: once reached the end of the
buffer, TraceBuffer stops accepting data.

There is a slight behavioural change from the V1 implementation. V1 tried
to be too smart about DISCARD and allowed to keep writing data into the buffer
as long as the read cursor was not hit (i.e. as long as the reader catched up).
This turned out to be useless and confusing: coupling `DISCARD` with
`write_into_file` leads to a scenario where DISCARD behaves like RING_BUFFER,
howver if the reader doesn't catch up (e.g. due to lack of CPU bandwith),
TraceBuffer stops accepting data (forever).
We later realized this was a misleading feature and added warnings when trying
to combine the two.

V2 doesn't try to do smart about readbacks and simply stops once we reach the
end of the buffer, whether it has been read back or not.

### Fragmentation

Packet fragmentation is the cause of most of TraceBuffer's design complexity.

TODO drawings

### Out-of-order commits

Out of order commits are rare but regularly present. They happen due to a
feature called _SMB Scraping_ introduced in the early years of Perfetto.

SMB scraping happens when TracingServiceImpl, upon a Flush(), forcefully reads
the chunks in the SMB, even if they are not marked as completed, and writes them
into the trace buffer.

The challenge is that TracingServiceImpl, when scraping, scans the SMB in linear
order and commits chunks as-found. But that linear order translates into
"chunk allocation order", which is unpredictable, effectively causing chunks
to be potentially committed out of order.

In practice, these instances are relatively rare, as they happen either only at
the end, or every O(seconds) in the case of long tracing mode. Hence they must
be supported, but not optimized for.

### Tracking of data losses

There are several paths that can lead to a data loss, and TraceBuffer must track
report all of them. Debugging data losses is a very common activity. It's
extremely important that TraceBuffer signals any case of data loss.

There are several different types and causes of data losses:

* SMB exhaustion: this happens when a TraceWriter drops data because the SMB
  is full. TraceWriters signal this by appending a special fragment of size
  `kPacketSizeDropPacket` at the end of the next chunk.
* Fragment reassembly failure: this happens when TraceBuffer tries to reassemble
  a fragmented packet and realizes there is a gap in the sequence of ChunkIDs
  (typically due to a chunk being overwritten in ring-buffer mode).
* Sequence gaps: this happens when there is no fragmentation but two chunks have
  a discontinuity in their ChunkID(s). This happens due to ring-buffer
  overwrites or due to some other issue when writing in the SMB.
* ABI violations: this happens when a Chunk is malformed, for instance:
  - One of its fragments has a size that goes out of bounds.
  - The first fragment has a "fragment continuation" flag, but there was no
    fragment previously initiated.

### Patches

When a packet spans across several fragments, almost always it involves patching
due to the nature of protobuf encoding. The problem is the following:

* A TraceWriter starts writing a packet at the end of a chunk.
* By doing so it starts writing a protobuf message (at very least for the root
  TracePacket proto). More protobuf messages might be nested and open while
  writing (e.g., writing a TracePacket.ftrace_events bundle)
* The TraceWriter runs out of space in the Chunk. So it commits the current
  chunk in the SMB and acquires a new one to continue writing.
* The chunk being committed contains the preamble with the message size. However
  that preamble at the moment is filled with zeros, because we don't know yet
  the size of the message(s), as they are still being written.
* Only when the nested messages end TraceWriter can possibly know the size of
  the messages to put in the preamble. But at this point the chunk containing
  the preamble has been committed in the SMB. TraceWriters cannot touch
  committed chunks. More importantly, they might have been already consumed by
  the TracingService.
* To deal with this, the IPC protocol exposes the ability to patch a Chunk via
  IPC, with the semantics: _If you (TracingService/TraceBuffer) still have
  ChunkID 1234_ for my `{ProducerID,WriterID}` patch offset X with contents
  `[DE,AD, BE,EF]`.

From a protocol viewpoint, only the last fragment of a chunk can be patched:

* Non-fragmented chunks don't require any patches.
* The first fragment of a chunk (which is the last of the chain) by design
  does not need any patching as there is no further fragment.
* Note that a chunk can contain a single fragment which is both the first and
  last, in the middle of a fragmentation chain. This can also need patching.
* In general given a packet fragmented in N fragments, all but the last
  fragment can (and generally will) need patching.

The information about "needs patching" is held in the SMB's Chunk flags
(`kChunkNeedsPatching`).

The `kChunkNeedsPatching` state is cleared by the `CommitData` IPC, which
contains, alongside the patches offset and payload, the `bool has_more_patches`
flag. When false, it causes the `kChunkNeedsPatching` state to be cleared.

From a TraceBuffer viewpoint patching has the following implications:

* Chunks that are pending patches cause a stalling of the readback, for the
  sequence.
* Stalling is not synchronous. TraceBuffer simply acts as if there was no more
  data for the sequence, either by moving on to other sequences or by returning
  false in ReadNextTracePacket() when all the other sequences have been read.
* Fragment reassembly stops gracefully in presence of a chunk that
  has patches pending, without destroy any data or signalling a data loss.
* Because patches travel over IPC, and the IPC channel is by design non-lossy
  we stall the sequence for arbitrary time in presence of missing patches.
* The stalling, however, cannot affect other sequences.
* So a fragmented packet missing patches can cause a long chain of packets (for
  the same TraceWriter sequences) to never be propagated in output when reading
  back, but cannot stall other sequences.
* However, if the chunks that are pending patches get overwritten by newer data,
  the stalling ends, and TraceBuffer shall keep reading next packets, signalling
  the data loss.

### Recommits

Recommit means committing again a chunk that exists in the buffer with the same
ChunkID.
The only legitimate case of recommit is SMB scraping followed by an actual
commit. We don't expect nor support the case of Producers trying to re-commit
the same chunk N times, as that unavoidably leads to undefined behaviour (what
if the tracing service has written the packets to a file already?).

This is the scenario when recommit can legitimately happen:

* SMB scraping happens, and TracingService calls CopyChunkUntrusted for a chunk
  that is still being written by the producer. While doing this it signals the
  condition to TraceBuffer passing the `chunk_complete=false` argument.
* The chunk is copied into the TraceBuffer. By design when committing a scraped
  (incomplete) chunk TraceBuffer ignores the last fragment, because it cannot
  tell if the producer is still writing it or not.
* Later on the TraceWriter (who is unaware of SMB scraping) finishes writing the
  chunk and commits it.
* TraceBuffer at this point overwrites the chunk, potentially extending it with
  the last fragment.

NOTE: kChunkNeedsPatching and kIncomplete are two different and orthogonal
chunk states. Incomplete has nothing to do with fragments.

Implications:

* An incomplete chunk causes a read stall for the sequence similar to what
  kChunkNeedsPatching does.
* Similarly to the former case, the stall withdrawn if the chunk gets
  overwritten.
* TraceBuffer never tries to read the last fragment of an incomplete chunk.
* As such an incomplete chunk cannot be fragmented on the ending side (phew).

The main complication of incomplete chunks is that we cannot know upfront the
size of their payload. Because of this we have to conservatively copy and
reserve in the buffer the whole chunk size.

### Buffer cloning

Buffer cloning happens via the CloneReadOnly() method. As the name suggests
it creates a new TraceBuffer instance which contains the same contents but can
only be read into. This is to support `CLONE_SNAPSHOT` triggers.

Architecturally buffer cloning is not particularly complicated, at least in
the current design. The main design implications are:

* Ensuring that nothing load-bearing in the TraceBuffer contains pointers.
  Any pointer data must be cleared before cloning to avoid UAF bugs.
* For this reason, the core structure in the buffer use offsets rather than
  pointers (which also happen to be more memory compact and cache-friendly).
* Stats and auxiliary metadata tends to be the thing that requires some care,
  where bugs can occasionally hide.

### ProtoVM

ProtoVM is an upcoming feature to TracingService. It's a non-turing-complete
VM language to describe proto merging operations, to keep track of the state
of arbitrary proto-encoded data structures as we overwrite data in the
trace buffer.
ProtoVM has been the reason that triggered the redesign of TraceBuffer V2.

Without getting into its details, the primary requirement of ProtoVM is the
following: When overwriting chunks in the trace buffer, we must pass valid
packets to ProtoVM. We must do so in order, hence replicating the same logic
that we would use when doing a readback.

Internal docs about ProtoVM:

* [go/perfetto-proto-vm](http://go/perfetto-proto-vm)
* [go/perfetto-protovm-implementation](http://go/perfetto-protovm-implementation)
* TODO(primiano): convert to .md and make them public.

### Overwrites

For the aforementioned ProtoVM reasons, in the V2 design, the logic that deals
with ring buffer overwrites (`DeleteNextChunksFor()`) is almost identical - and
shares most of its code with - the readback logic.

I say almost because there is (of course) a subtle difference: when deleting
chunks, stalling (either due to pending patches or incompleteness) is NOT an
option. Old chunks MUST go to make space to new chunks, no matter what.

So overwrites are the equivalent of a no-stalling force-delete readback.

## Core design

TODO describe here.
Draw diagram with chunks out of order to explain

### Core interactions

![core-interactions](/docs/images/tracebuffer-design/core-interactions.drawio.svg)

## Implementation details

### Structures and helper classes

#### `TBChunk`

![tbchunk](/docs/images/tracebuffer-design/tbchunk.drawio.svg)

Is the struct, stored in the trace buffer memory as a result of calling
CopyChunkUntrusted from a SMB chunk.

A TBChunk is very similar to a SMB chunk with the following caveats:

* The sizeof() both is the same (16 bytes). This is very important to keep
  patches offsets consistent.

* The SMB chunk maintains a counter of fragments. TBChunk instead does
  byte-based bookkeeping, as that reduces the complexity of the read iterators.

* The layout of the fields is slightly different, but they both contains
  ProducerID, WriterID, ChunkID, fragment counts/sizes and flags.
  The SMB chunk layout is an ABI. The TCHunk layout is not: it is an
  implementation detail and can be changed.

* TBChunk maintains a basic checksum for each chunk (used only in debug builds).

### SequenceState

It maintains the state of a `{ProducerID, WriterID}` sequence.

Its important feature is maintaining an ordered list (logically) of TBChunk(s)
for that sequence, sorted by ChunkID order.
The "list" is actually a CircularQueue of offsets, which has O(1)
`push_back()` and `pop_front()` operations.

The lifetime of a `SequenceState` has a subtle tradeoff:

* On one hand, we could destroy a SequenceState when the last chunk for a
  sequence has been read or overwritten.
* After all we must delete SequenceState(s) at some point. Doing otherwise would
  cause memory leaks in long running traces if we have many threads coming and
  going, as up to 64K sequences per producer are possible.
* On the other hand, deleting sequences too aggressively have a drawback: we
  cannot detect data losses in long-trace mode
  (see [Issue #114](https://github.com/google/perfetto/issues/114) and
  [b/268257546](http://b/268257546)). [Long trace mode][lt] periodically
  consumes the buffer, hence making all sequences eligible to be destroyed if we
  were to be aggressive.
* The problem here lies in the fact that `SequenceState` holds the
  `last_chunk_id_consumed` which is used to detect gaps in the chunk ids .

TraceBufferV2 balances this using a lazy sweeping approach: it allows the most
recently deleted `SequenceState`s to stay alive, up to
`kKeepLastEmptySeq = 1024`. See `DeleteStaleEmptySequences()`.

### FragIterator

A simple class that tokenizes fragments in a chunk and allows forward-only
iteration.

It deals with untrusted data, detecting malformed / out of bounds scenarios.

It does not alter the state of the buffer.

### ChunkSeqIterator

A simple utility class that iterates over the ordered list of TBChunk for
a given SequenceState. It merely follows the SequenceState.chunks queue
and detects gaps.

### ChunkSeqReader

Encapsulates most of the readback complexity. It reads and consumes chunks
in sequence order, as follows:

* When constructed, the caller must pass a target TBChunk as argument. This is
  the chunk where we will stop the iteration *.
* At readback time this is the next chunk in the buffer that we want to read.
* At overwrite time this is the chunk that we are about to overwriter.
* In both cases, because of OOO commits, the next chunk in buffer-order might
  not necessarily be the next chunk that should be consumed in FIFO order
  (although in the vast majority cases we expect them to be in order).
* Upon construction, it rewinds all the way back in the `SequenceState.chunks`
  (using `ChunkSeqIterator`) and starts the iteration from there.
* It keeps reading packets until we reach the target TBChunk passed in the
  constructor.
* In some cases (fragmentation) it might read beyon the target chunk. This is
  to reassembly a packet that started in the target chunk and continued later
  on.
* When doing so it just consumes the fragment required for reassembly and leaves
  the other packets in the chunk untouched, to preserve global FIFO-ness.
