# TraceBuffer V3: TraceBuffer V2 * 2 + Bundling

**Authors:** @sashwinbalaji

**Status:** Draft

This RFC describes the layoyut and flow of TraceBuffer V3 (TBv3). Very high level, TBv3 is tracing service writes data into trace buffer as of now for both tracing service V1 and V2 (ongoing), then periodically we read these chunks convert them into trace packet, bundle and compress them and write them into final trace buffer where they live until read out by consumer.

Note: On why we chose to go with ZSTD-3 as the compression alog with 1MB as compression trigger threshold see [RFC-38](https://github.com/google/perfetto/discussions/6397)

## Requirements
- Support both SMB v1 and v2 chunks. V1 chunks bring in the most complexity of needing to support out of order chunks, scraping, patches.
- No behavior change for caller and callee of trace buffer. TBv3 from outside should just look like TBv2 or TBv1. The bundling into chunks, the new header format (discussed below) are all hidden. ReadBuffers should return the same trace packet as it does with the same fields. None of this should leak into trace filter or trace processor.
- Work just like a opt in feature/mode of TBv2 meaning support everything TBv2 does of protomvm, cloning and fill policies.

## Proposed TBv3

```
                          producers
                 |                         |
        SMB v1 chunks              SMB v2 ring chunks
   (CommitData IPC, SMB scrape)     (ring buffer drain)
                 |                         |
       CopyChunkUntrusted()      CopyChunkV2Untrusted()
                 v                         v
  +------------------------------------------------------------+
  | staging buffer: TBv2 as it is today               size_kb  |
  | chunks, fragments, patches, out-of-order commits           |
  +------------------------------------------------------------+
                 |
                 |  EndWrite(), at the end of each write batch.
                 |  Once 1 MiB of new data has arrived:
                 |    1. read the complete packets (cur |ReadNextTracePacket|)
                 |    2. compress them into bundles
                 |    3. Convert each bundle into TBv3 chunk (bundle + header)
                 |    4. compact staging
                 v
  +------------------------------------------------------------+
  | bundle buffer: a plain ring of bytes              size_kb  |
  |                                                            |
  |    oldest                                      newest      |
  |    [bundle][bundle][bundle][bundle][bundle][bundle]        |
  |    ^ reads and drops happen here      appends happen here ^|
  +------------------------------------------------------------+
                 |
                 |  ReadBuffers(): bundles, oldest first,
                 |                 then what is left in staging
                 v
   trusted fields -> trace filter -> final compression -> trace
```

So TBv3 has two TBv2 buffers we call them:
- Staging buffer: 
    - This is current TBv2 with same write and read path. It contains all the complexities of SMB v1 and v2 chunks. 
    - This is the final place in SMB V2 flow where "perfetto proto group" exists once the trace packet leaves this buffer it's converted into its normal form.
- Bundled buffer:
    - This is a pure ring buffer, doesn't need to worry about padding and patches.
    - A bundle written in it is complete in itself


Few (slightly obvious) things which hold:
- At any state, every packet is in either of the two buffers
- The packet in bundled buffer is older than the one in staging.
- Once a bundle is made, it doesn't change and nobody from outside hold a pointer to it.

## When does chunks/bundles move across components ?

### Packets coming into staging:

No change in this flow. The chunks will come into staging buffer just as today via CopyChunkUntrusted and the newly added CopyChunkV2Untrusted.

This functions are invoked by tracing service in these scenraios:
1) V2: DrainBuffer -> CopyChunkV2Untrusted
    The normal flow in SMV V2 of prodcuer telling service to read its SMB
2) V1: CommitData -> CopyProducerPageIntoLogBuffer -> CopyChunkUntrusted
    The normal flow in SMV V1 of producer telling service to read its SMB
3) V1: ScrapeSharedMemoryBuffers -> ForEachScrapableChunk -> CopyProducerPageIntoLogBuffer -> CopyChunkUntrusted
    The scraping case where the chunks 

### Bundles moving from staging to bundled:

We'll configure a certain threshold bytes (compact_threshold_bytes) correspondign to the amount of staging buffer filled when we'll compress and move it to bundled buffer.

Now the question is where exactly we check if the threshold is crossed to do the transfer.

Doing it via a separate PostTask is just unnecessary complexity, doing it at read time would be too late as at that point why not just bypass bundling and write to file and you won't get the benefit of able to "write more". So the best place seems to be doing it at write time itself and it should be no different than dpoing a periodic readback.

Now the first obvious idea is to do it within CopyChunkxxUntrusted. Simply monitor how much chunks have been copied and if greater than compact_threshold_bytes, read the currently written chunks and move them to bundling buffer.

But with V1 chunks that won't work because: 

1) Scraping means chunks can be out of order 
    - During scraping we scan the SMB in linear order and commit chunks as found. But that linear order translates into "chunk allocation order", which is unpredictable, effectively causing chunks to be committed in random order.
    - In TBv2 we resolve this by sorting the chunks by its ID and rely that all out-of-order commits are batched together atomically before any read.
    - So now in scraping flow we repeatedly call CopyChunkUntrusted and if in any of the intermediate calls we trigger read of the existing chunks for bundling then we'll miss the later in physical order but earlier in chunk order chunk order cnhunks unnecessarily

    e.g.

```
  SMB of P, in page order:   page 2: chunk 7 of W
                             page 9: chunk 6 of W

  Step inside CopyChunkUntrusted():

    CopyChunkUntrusted(W, chunk 7)
       1 MiB is reached, so the step runs now.
       It reads W. After chunk 5 it finds chunk 7.
       That looks like a hole. The packets of chunk 7 go out
       with a loss flag.
    CopyChunkUntrusted(W, chunk 6)
       Chunk 6 is older than the last chunk read, which is 7.
       TBv2 drops it.
```

2) (Minor issue) Patches are copied after chunk
    - CommitData handls list of chunks followed by list of patches (ApplyChunkPatches). And we copy the chunks first and then apply (TryPatchChunkContents) there corresponding patches.
    - Now if a packet is split across we won't be able to read it until the chunk's patch is applied.
    - That means handling bundling within CopyChunkUntrusted would mean leave all these chunks unnecessarily in staging
    - This will limit how much can be bundled unnecesswrily.


***Proposal: Introdcue EndWrite function***

- Just like we recently introduced the notion of EndRead to let service indicat trace buffer it has read everuthong and buffer cna alter the state, we need a new end write function for servivce to tell the buffer it has completed its write cycle.
- This solves both the above problems but need to plugged in all the three places currently calling copy chunks.
- When it completes Every packet is either in staging or in a sealed bundled chunk. 

## What does moving from staging to bundling involve ?

1) EndWrite function invoked
    - we run the current read path on staging: BeginRead and then ReadNextTracePacket in a loop until it returns false. This is literally the readback code so we get everything it does today for free:
        - fragments are reassembled into whole packets
        - a chunk waiting for a patch is skipped, its whole sequence waits
        - a scraped incomplete chunk gives its complete packets, the tail waits for the real commit
        - V2 packets come out rewritten from proto groups to normal protobuf
        - each packet comes with its sequence (producer id, writer id, uid, pid, machine id) and the loss flags of that sequence
        - per sequence order is kept and across sequences we get buffer order
2) Each packet we get back goes straight into zstd.
    - Streaming, so there is no uncompressed copy of the bundle anywhere. zstd writes into a scratch vector that we reuse across bundles.
    - In front of each packet we write a small header and keep track of prodcuers and writers in that bundle (details below).
3) When bundle reaches a threshold (below) we seal it.
    - Finish zstd, build the header.
    - If in RING_BUFFER mode, drop the oldest bundled chunks from the bundled buffer to make space and then memcpy header + tables + compressed bytes in.
4) Compact staging.
    - After the read most of staging is padding or the chunks that could not move. So at this point we invoke "EndRead" func to do the left shift compaction of the staging buffer.

Bundle threshold is reached when either:
- ReadNextTracePackets returns false nothing more to read
- Bundle size has grown to 2x compact_threshold_bytes. We trigger bundling when we see there are atleast compact_threshold_bytes but it's possible by the point we trigger there is way too much to bundle and to minimise the overall bundle size we add this cap.

## Bundled buffer layout

The bundled buffer is one flat piece of memory of size_kb bytes. The things stored in it are called BundledChunks (next section). They have different sizes and sit side by side with nothing in between.

```cpp
class BundledBuffer {
  base::PagedMemory mem_;  // size_kb bytes
  uint64_t head_ = 0;      // where the next BundledChunk goes
  uint64_t tail_ = 0;      // where the oldest BundledChunk starts
};
```

head_ and tail_ are not offsets into the memory. They are positions in the stream of all BundledChunks ever written, starting at 0 and only going up. The byte at position p lives at `p % mem_.size()` in the memory. tail_ is the position of the oldest BundledChunk we still have, head_ is where the next one will go, and `head_ - tail_` is how many bytes are in use.

They are uint64_t and count bytes, so they never wrap in practice:
- head_ overflows when it reaches 2^64 = 1.8 x 10^19 bytes.
- Assume 1 GB/s of compressed output, 10^9 bytes per second. That is far above reality, a heavy trace compresses to about 1 MB/s.
- Seconds to overflow: 1.8 x 10^19 / 10^9 = 1.8 x 10^10.
- One year is 365 x 24 x 3600 = 3.15 x 10^7 seconds.
- Years to overflow: 1.8 x 10^10 / 3.15 x 10^7 = ~585 years. At the realistic 1 MB/s it is ~585,000 years.

No traced process lives that long, so we can say "they only grow" and keep `tail_ <= head_` as a plain rule with no wrap handling. (RFC-0046 uses the same positions idea for the SMB V2 ring, but with 32 bits, which is why that ring needs a wrap count and we do not.)

```
        tail_                               head_
          v                                   v
  ...-----+--------+--------+--------+--------+
          |   B3   |   B4   |   B5   |   B6   |
  ...-----+--------+--------+--------+--------+

  the same bytes in memory:

  0                                             mem_.size()
  +-------+------------+--------+--------+--------+-------+
  | B6    |    free    |   B3   |   B4   |   B5   | B6    |
  | (end) |            |        |        |        |(start)|
  +-------+------------+--------+--------+--------+-------+
          ^ head       ^ tail
```

What holds at all times:
- `tail_ <= head_` and `head_ - tail_ <= mem_.size()`
- tail_ is at the start of a BundledChunk or equal to head_
- each BundledChunk starts where the previous one ends. No alignment, no padding, no gap.
- nothing lives in the memory except BundledChunks. The two positions and the read offset live in the C++ object, same as wr_ in TBv2.

### How a BundledChunk gets appended

Say capacity is 2.0 MB, positions 0 to 1.9 MB are in use, the oldest BundledChunk B1 is 0.3 MB, and a new BundledChunk of 0.2 MB arrives.

```
 before:      tail_ = 0                               head_ = 1.9
 memory:      |  B1  |   B2   |   B3   |  ...   |      | free |
              0     0.3                               1.9    2.0
```

1) Room check: free is 2.0 - 1.9 = 0.1 MB. Not enough.
2) Overwrite the oldest BundledChunk, B1, as a whole: read its header and writer table, flag its sequences, feed protovm if needed, `tail_ += 0.3`. Free is now 0.4 MB, enough. If B1 had been smaller we would keep going with B2 and so on until it fits. Always whole BundledChunks, oldest first, never a part of one.
3) Write the new one at head_ = 1.9. In positions it is 1.9 to 2.1. In memory that is two pieces: 0.1 MB at offsets 1.9 to 2.0, then 0.1 MB at offsets 0 to 0.1 where B1 used to be. Two memcpys.
4) `head_ = 2.1`.

```
 after:            tail_ = 0.3                        head_ = 2.1
 memory:      | new | free |   B2   |   B3   |  ...   |   new    |
              0    0.1    0.3                         1.9       2.0
               (end)                                   (start)
```

Things to notice:
- Room is made first, then we write. There is never a moment where a new BundledChunk sits on top of part of an old one that still counts as live.
- We freed 0.3 MB but needed only 0.1 MB. The leftover stays free until the next BundledChunk uses it. A half BundledChunk cannot be read, so the unit of overwrite is the whole thing.
- The new BundledChunk is one thing in position space. Only the memory view is split, and that is why we think in positions and not in memory offsets.
- This "overwrite the oldest" is exactly what RING_BUFFER means and what TBv2 does today chunk by chunk. Once the buffer is full, which is most of a ring buffer trace, every new BundledChunk pushes an old one out. So the overwrite path runs as often as the seal path and must stay cheap: read the tables, flag the sequences, move tail_. (That is why the tables are kept outside the compressed bytes, next section.)

A BundledChunk can continue past the end of the memory at offset 0 (B6 above, and the new one in the example):
- a write is at most two memcpys, reading the header or tables goes through one small copy helper
- zstd's streaming decode takes the two pieces one after the other, that API exists for input that arrives in pieces
- the other option is to leave the end unused when a BundledChunk doesn't fit, like TBv2 does with a padding chunk. That brings back a gap the reader has to know about. On average half a bundle wasted, and after a single packet bundle it can be most of the buffer. Not doing that.

Operations:

| Operation | What it does | Cost |
|---|---|---|
| Append | memcpy at head_, head_ += its size | one copy of the BundledChunk |
| Overwrite oldest | read header + writer table at tail_, tail_ += its size | a few hundred bytes read |
| Read oldest | decode the compressed bytes at tail_ into a read buffer | one decompression |
| Room left | `mem_.size() - (head_ - tail_)` | one subtraction |

- Every operation above needs "its size": the length of the BundledChunk at tail_ or the one being written. That is the `stored_size` field in the header, next section.
- A BundledChunk larger than mem_.size() is refused. Because of the bundle size cap this can only be a single huge packet.
- In RING_BUFFER append overwrites oldest first until the new one fits.

Why a plain ring pays off:
- No index. The header at tail_ says where the next one starts. The buffer is its own index.
- No state per sequence. A BundledChunk carries its own tables, when it goes its tables go with it.
- No allocation while tracing. Memory is reserved once at creation, if that fails EnableTracing fails cleanly.
- Memory use is known up front and never changes, which is what the watchdog guardrail wants (see Memory).
- Clone is a memcpy of bytes, no pointers to fix.
- EndRead can give memory back with the same helper staging uses.
- Any BundledChunk stands alone, it can be decoded or overwritten without looking at another one.
- Easy to check. A test can walk tail_ to head_ header by header and verify the whole buffer.

## Bundled chunk design

One single unit in bundled buffer is a "BundledChunk". 

It is complete in itself: with a header saying how big it is and whose packets are in it, and the bundle, which is the compressed packets.

```
  +--------+-----------+---------+----------------------------------+
  | header | producers | writers | bundle (compressed bytes)        |
  +--------+-----------+---------+----------------------------------+
    12 B     16 B each   4 B each  one zstd block
```

```cpp
struct BundleHeader {          // 12 bytes
  uint32_t stored_size;        // Total size of BundledChunk = header + tables + compressed bytes
  uint32_t uncompressed_size;  // the compressed bytes once decoded
  uint16_t num_producers;
  uint16_t num_writers;
};

struct ProducerEntry {         // 16 bytes, who a producer is
  uint16_t producer_id;
  uint16_t unused;             // zero
  uint32_t uid;
  int32_t pid;
  uint32_t machine_id;
};

struct WriterEntry {           // 4 bytes, one sequence
  uint16_t producer_id;
  uint16_t writer_id;
};
```

### Why some metadata kept uncompressed ?

The bytes outside zstd are the "expensive" but are worth it:

- A dropped bundled chunk must tell us which sequences it had. If the tables were inside the compressed bytes every drop would decompress ~1 MiB to read 2 KB.
- Compressing the tables as a block of their own might help but trades off some minimal space saving for a second compression at seal plus a decompression at every drop. Can be done separately if needed later on.

### Header

- `stored_size`: the length of this BundledChunk, the equivalent of `TBChunk::size` in TBv2. The layout section above needs it everywhere: to step from tail_ to the next BundledChunk, to know how much to memcpy, to know how much room a new one needs. The header and the tables have known sizes but zstd's output does not say how long it is. Finding its end would mean walking zstd's internal blocks, and that needs the bytes in one piece, which they are not when a BundledChunk wraps around the end of the memory. So we write the total once at seal.
- `uncompressed_size`: how many bytes the compressed bytes turn into. The reader needs it to allocate the read buffer before calling zstd, and after decoding it checks that zstd produced exactly this many bytes. zstd can store this number itself, but only if it is told the total before compression starts. We stream packets in one by one and only know the total at the end, so we store it ourselves.
- `num_producers`, `num_writers`: how long the two tables are.
- No magic, no version, no checksum of our own:
    - TBv2 has a checksum per chunk because it reaches chunks via many stored offsets (wr_, rd_, every SequenceState::chunks list). A stale offset would read random bytes as a header and the checksum catches that.
    - Bundled buffer reaches a bundled chunk in exactly one way: tail, then tail + stored_size. There is no other offset that can go stale.
    - It still checks itself for free: every zstd block starts with a fixed 4 byte magic. A CHECK on that magic plus `stored_size <= head - tail` catches a wrong size the same way TBv2's checksum catches a wrong offset.
    - No version because this format lives in the memory of one traced process. It never goes to a file or over IPC, so it never crosses a version boundary and we can change it any time.
    - No malicious input to worry about. Producers never write here, traced does, from packets TBv2 already cut at fragment boundaries. The packet bytes inside are still untrusted as content and go through PacketStreamValidator at readback exactly like a TBv2 chunk does today.

### Producer table

One entry per producer that has a packet in this bundle: its id and the identity (uid, pid, machine id) that service stamps on its packets.

- The identity belongs to the producer, every writer of that producer shares it.
- TBv2 stores it per sequence (in SequenceState) and can afford to, it has one SequenceState per sequence for the whole buffer. A bundled chunk is a snapshot and ~50 of them are alive in a 16 MiB buffer. Storing identity per sequence would repeat the same 12 bytes in every bundled chunk for every writer.
    - 300 writers: 6 KB per bundled chunk. 1000 writers: 20 KB, that is ~7% of the ring.
    - With identity once per producer the two tables together are ~2 KB and ~6 KB for the same cases.

### Writer table

One entry per sequence that has a packet in this bundle. The entry is just the sequence key TBv2 uses everywhere (ProducerAndWriterID) written out as its two halves.

- Dropping a bundled chunk reads this table and nothing else: it has every sequence to flag and every producer id for the protovm check (protovm only needs the producer id, verified in MaybeProcessOverwrittenPacketWithProtoVm).
- Readback joins the two tables once when it opens a bundled chunk, after that every packet is one array lookup.


### What is inside the compressed bytes

```
  compressed bytes --zstd--> the packets, each with a small header:

  [writer idx][loss][size][packet bytes][writer idx][loss][size][packet bytes]...
    varint     varint varint
```

- `size`: a protobuf message does not say where it ends. Every place that stores packets puts a length in front, the trace file, the SMB, TBv2 chunks. This is the same length the trace file uses minus the 0x0A tag. The tag only means something to a parser of the Trace message and nothing parses these bytes as one. Service puts the tag back when writing the file, as it does today.
- `writer idx`: index into the writer table. This is what the TBv2 chunk header used to tell us, whose packet this is. At readback service stamps trusted_packet_sequence_id, uid, pid, machine id from it on every packet. Protovm takes the producer id from it.
- `loss`: the exact value TBv2 returns with the packet today (previous_packet_on_sequence_dropped), a DataLossReason bitmask. 0 means nothing lost. Nonzero means a gap is right before this packet on its sequence and the bits say why. Trace processor uses it to invalidate the sequence's interned data and to count the loss. TBv2 forgets the flag the moment it returns the packet so the bundle has to carry it. One byte when zero.
- Every packet has the same three varints in front, no other kind of entry, no value with a special meaning. Decoder reads three varints and takes size bytes. ~3 bytes per packet before compression, far less after.
- No trusted fields are stamped at bundling time:
    - trusted_packet_sequence_id is numbered per session and a clone starts a new numbering. Packets stamped in the source session would clash with packets stamped in the clone.
    - A bugreport clone skips the trace filter so bundles must hold unfiltered packets and filtering stays at readback.
- zstd settings: one block per bundle, level 3, content size and checksum flags off (header has the size, a decode error is a CHECK).

Cost for a typical bundled chunk (300 KB compressed, 6000 packets, 300 writers from 40 producers):

| Part | Bytes | Share |
|---|---|---|
| Header | 12 | 0% |
| Producer table | 640 | 0.2% |
| Writer table | 1200 | 0.4% |
| Packet headers before compression | ~18 KB | well under 1% after compression |

## Data loss

Three ways to lose data, each flagged on the next packet of that sequence:

| What is lost | Where the pending flag lives | Which packet reports it |
|---|---|---|
| chunks in staging (overwritten, missing, malformed) | SequenceState in TBv2, as today | next packet read from staging |
| a bundled chunk dropped from the bundled buffer | a map sequence -> loss flags | next packet the reader returns for that sequence |
| one packet too large for the bundled buffer | SequenceState of its sequence | next packet read from staging |

- Staging loss travels with the packet. The move gets the flag from the staging read and writes it into the packet's small header. Reader finds it there later.
- Dropping a bundled chunk marks every sequence in its writer table in the map. The reader ORs the mark into the next packet it returns for that sequence and erases it. That packet can come from a bundled chunk or from staging.
- The move never looks at the map. The packet it moves is not always the next one a reader will see for that sequence, an older one may still sit in a bundled chunk.
- Dropping a half read bundled chunk marks only the sequences that still had unread packets in it.
- Why the flag always lands after the gap: reads and drops both take from tail_. So everything a reader returns later is newer than everything that was dropped. No bundle ids, no timestamps needed.
- Limit: the map holds at most 64K sequences. Past that drops are counted in stats but not flagged per sequence. TBv2 has the same kind of limit today, it keeps loss state for the last 1024 idle sequences (kKeepLastEmptySeq).

## Protovm

- Today protovm gets the packets TBv2 overwrites, at the moment they are overwritten.
- In TBv3 a packet is dropped when its bundled chunk is dropped, so that is when protovm gets it:
    1) if no VM wants a producer in the writer table, nothing is decoded
    2) otherwise decode the bundled chunk into a scratch buffer
    3) each packet of a wanted producer goes to MaybeProcessOverwrittenPacketWithProtoVm, same function TBv2 uses today
- If the dropped bundled chunk was half read only the unread packets go to the VM. Packets a consumer already took are never given to it, as today.
- This is the only place that feeds protovm in TBv3. Packets reach it in order and as normal protobuf.
- Staging overwrites do not feed it in TBv3:
    - staging overwrites unread chunks only when one write batch is larger than the free space in staging
    - those packets are newer than the ones waiting in the bundled buffer, giving them to the VM first would be out of order
    - so in TBv3 they are plain loss, flagged as today
- Cost: with a VM every dropped bundled chunk that has a wanted producer costs one decode plus the VM run, inside EndWrite.
- SMB V2 + protovm: today TBv2 rejects V2 chunks from a producer that has a VM on the buffer because the overwrite path can't turn proto groups into normal protobuf. TBv3 keeps that rule for now. But in TBv3 the VM only ever sees packets from bundles which are already normal protobuf, so the reason is gone and we can lift it later.

## Clone

- CloneReadOnly copies: staging as TBv2 does today, the used bytes of the bundled buffer placed at the start of the new memory, the read offset inside the oldest bundled chunk, the loss map.
- No decoded data is copied, the clone decodes again when read.
- Service needs no extra call before a clone, there is no open bundle to close.
- A clone never runs EndWrite, it is read only.
- transfer_on_clone moves the buffer to the cloned session and the source gets a new empty TBv3. clear_before_clone recreates the buffer as TBv3.
- Today three places in service create buffers and switch on the type (EnableTracing, clear_before_clone, transfer_on_clone). They move into one CreateTraceBuffer helper.

## Fill policies

### RING_BUFFER

- Bundled buffer drops its oldest bundled chunks, trace keeps the newest data.
- Staging rarely fills because every move empties it. If one write batch is bigger than the free space in staging, staging overwrites its oldest unread chunks as TBv2 does and flags the loss. Older bundles survive, so the trace has old data, a flagged hole, then new data.
- A chunk that never completes stays at the front of staging and only goes away when staging really fills and wraps. This comes with compaction and is the same for plain TBv2.

### DISCARD

DISCARD keeps the oldest data and stops when full. In TBv3 the session stops at whichever buffer fills first (meeting). Three rules:

1) Staging stops the way TBv2 stops: refuses the first write that doesn't fit before the end of the buffer and every write after it. Compaction moves wr_ back after each move, so staging reaches its end only when it is full of data that could not move. No new rule needed in staging.
2) A move starts a bundle only if the largest possible bundled chunk fits (max_bundle_bytes + zstd worst case growth + full tables). If not, bundling stops for good and the packets stay in staging. Then rule 1 takes over, staging fills up and stops.
3) A single packet larger than max_bundle_bytes is checked with its own size. If its bundled chunk might not fit the packet is lost and flagged and bundling stops.

- Trace is then the bundled chunks followed by staging, oldest data with no hole except for the one packet of rule 3.
- Rule 2 can leave up to ~max_bundle_bytes of the bundled buffer unused.


## Stats

New BufferStats fields (ids after the one #7750 adds):

| Field | Meaning |
|---|---|
| bundles_written | bundled chunks stored |
| bundle_bytes_uncompressed | packet bytes with their small headers, before compression |
| bundle_bytes_compressed | stored bytes of those bundles |
| bundles_overwritten | bundled chunks dropped to make room |
| bundles_discarded | bundled chunks that could not be stored |
| bundle_step_max_us | the longest EndWrite |

- Compression ratio = bundle_bytes_uncompressed / bundle_bytes_compressed.
- Existing fields keep describing staging. In TBv3 chunks_overwritten > 0 means staging overflowed.
- Trace processor needs to import the new fields.

## Cost on the service thread

- Everything runs on the service thread, reads writes and moves never overlap.
- One EndWrite does at most the work for one write batch plus 1 MiB. A write batch is bounded by the size of the producer's SMB.
- Producer does not wait for it. CommitData gives each SMB chunk back to the producer right after copying it, before EndWrite runs. Same for the V2 drain.
- While a move runs the service thread does nothing else, other producers keep writing into their own SMB.
- Measured: prototype on x86 host at 2.6 GHz read from staging and compressed with zstd 3 at ~160 MB/s, so ~6-7 ms for a 1 MiB move. Not measured: a device. The device run has to confirm one move in one go is fine on a little core.
- A huge packet makes one long move, a 64 MiB heap dump is compressed in one call. Splitting that needs a copy of the packet, first version doesn't do it.

## Open questions

### A scraped chunk that the producer commits later

What happens today with SMB v1:

1) A trace writer fills a chunk in the SMB and marks it complete. The producer does not tell the service right away, it batches the commit and sends the CommitData IPC a bit later.
2) If a flush happens in that window, the service scrapes the SMB. It copies every chunk it finds, including this complete but not yet committed one, into TBv2 via CopyChunkUntrusted.
3) The producer's CommitData for that same chunk arrives later. The service calls CopyChunkUntrusted again for the same chunk id. This is a re-commit.

So after a scrape the same chunk reaches TBv2 twice. TBv2 knows this and handles it:

- If the first copy is still in the buffer, TBv2 finds it in the writer's chunk list, sees the payload is identical and skips the second copy quietly (`trace_buffer_v2.cc:977-979`).
- If the first copy was already read, the chunk is gone from the buffer. TBv2 only remembers "the last chunk I consumed for this writer was id 7". The second copy says "here is chunk 7". Same id, and it was not an incomplete chunk, so there is nothing new in it. TBv2 drops the copy. That is the right result, the data was already delivered.
- But that branch (`trace_buffer_v2.cc:872-884`) also does two more things: it adds one to `chunks_discarded`, and it has `PERFETTO_DCHECK(suppress_client_dchecks_for_testing_)`, which means "this only happens with a buggy producer". In a debug build it crashes.

Why this is rare in TBv2 today:

- The second case needs a read to land between the scrape and the commit. Reads happen at the end of the trace, or every few seconds with write_into_file.
- With write_into_file the periodic read does a flush first and the flush scrapes, so the window is real there. Everywhere else it is rare.

Why TBv3 hits it all the time:

- A scrape is a write batch, so it ends with EndWrite. If the threshold is reached, the move reads the scraped chunk right there, before the real commit shows up.
- Even if that EndWrite moves nothing, the next EndWrite from any other producer's commit can, and it will often come before this producer's commit.
- Flushes happen at every periodic flush, every clone or bugreport and at stop. So "scrape, move, then the real commit arrives" becomes the normal sequence after every flush, for every writer that had a complete uncommitted chunk at that moment.

What goes wrong and what does not:

- No data is lost. The chunk was bundled from the scraped copy, dropping the duplicate is correct.
- `chunks_discarded` goes up for something that is not a discard. People read that counter as "data was dropped", and in TBv3 we also want it to mean "staging overflowed".
- Debug builds crash on the DCHECK for a producer that did nothing wrong.

Proposed fix:

- Recognize the case for what it is: same chunk id as the last consumed one, that chunk was complete, and the new copy is not bigger. That is a duplicate of something already delivered. Return without touching the counter and without the DCHECK, or count it under its own name.
- TBv2 already has the sibling case right next to it: the re-admit branch for a scraped incomplete chunk that comes back with more data. This is the same shape for a scraped complete chunk that comes back unchanged.

