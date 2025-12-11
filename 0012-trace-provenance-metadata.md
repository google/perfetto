# TraceProvenance packet: Adding Producer, sequence and buffer mappings in the trace

**Authors:** @primiano

**Contributors:** @rukkal

**Status:** Draft

## Problem

1) When ProtoVM is run offline in TraceProcessor, it needs to be able to map
   a TracePacket (from its trusted_sequence_id) to the ProducerID, to then look
   up the right VM instance.

2) Anecdotally we have troubles debugging trace contents because we lose a lot
   of the information about buffers and data sources, leaving us with a lot of
   guesswork.

This proposal is to catch both birds with one stone.

This proposal does not address 100% the provenance problem, but makes the
situation significantly better. The mappings between data sources and sequences
remains still unsolved. However this RFC offers a solution for mapping sequences
to producers (i.e. processes).

## Proposal

Add a new packet emitted by TracingServiceImpl that reports

1) The list of all (*) remote machines / VMs
   - NOTE: We should also add this to `perfetto --query` / TracingServiceState

2) The list of all (* see Open Questions) producers (like in `perfetto --query`)
    - For each producer:
    - The Producer ID
    - The Machine ID
    - The name (process name)
    - uid and pid (can be useful to cross-check with /proc dumps)
    - sdk version

3) The list of data sources and mappings to the producers:
    - Producer ID
    - The whole [DataSourceDescriptor](https://github.com/google/perfetto/blob/main/protos/perfetto/common/data_source_descriptor.proto). This will include things like GpuCounterDescriptor and the available track event categories

4) The list of buffers and writer mappings
  - For each buffer
    - A repeated pair of `<trusted_sequence_id, ProducerID>`

Note that this will NOT give a 100% complete picture, as we still don't have
reliable mapping between data sources and sequences. So in the grand scheme of

```
  Producer --> Data Source  --> Buffer --> TraceWriter (trusted_seq)  --> TracePacket
```

With this proposal we will have the following mappings:

#### TracePacket <-many:1-> Producer

For any TracePacket in the trace we can tell reliably which producer emitted it
(but not which data source within the producer).

In the other direction, given a Producer we can tell all the packets it emitted.

#### TracePacket <-many:1-> Buffer

For any TracePacket in the trace we can tell reliably which buffer it was
targeting:
 - we know which sequence ID the packet belongs to.
 - we know which buffer contains that given sequence ID.
 - sequence IDs are unique: a seq ID is a combination of (Producer,TraceWriter)
   and by protocol design, a TraceWriter can only ever write into one buffer.

Vicevrsa. For any given buffer we can tell how much each TracePacket contributes
to its usage.

#### Buffer <-many:1-> Producer

For each buffer we can tell the contribution of each producer (and viceversa)

### Keeping track of changes over time

An open problem is the fact that producers and data sources can come and go
during a tracing session. The proposal here is:

- Short term: only grab a snapshot at the beginning (or end, TBD) of the trace.
- Long term: every time a data source connects/disconnects record the timestamp
  of the event in a per-trace-session buffer, so we have the full history
  (however can leak on long traces, maybe have some limits, TBD).

## Detailed design

A new `TraceProvenance` packet is emitted by `TracingServiceImpl`. This packet
contains the mappings between producers, sequences, and buffers.

### Proto schema

```protobuf

message TracePacket {
  oneof data {
    ...
    optional TraceProvenance trace_provenance = ...;
  }
}


message TraceProvenance {
  # NOTE: we already get remote machines in SystemInfo as per #1133.

  # TODO refactor to have a common message shared with TracingServiceState.
  message Producer {
    # The ProducerID. This is a 16-bit monotonic counter. There is one id per
    # IPC socket connection.
    optional uint32 id = 1;
    optional string name = 2;
    optional uint32 machine_id = 3;
    optional int32 uid = 4;
    optional int32 pid = 5;
    optional string sdk_version = 6;

    # If this field is 0/absent, the producer was already connected when we
    # started tracing. If > 0, this records the time when the it connected.
    optional int64 connect_time_ns = 7;

    # As above. If the field is absent, the producer was still connected when
    # we stopped tracing. If present, records the time when it disconnected.
    optional int64 disconnect_time_ns = 8;
  }

  # All producers connected to the tracing service.
  # See open question Q1 about whether this includes all producers
  # or only those active in the current trace.
  repeated Producer producers = 2;

  # TODO if a data source is instantiated twice in the same config, do we have 
  # 1 entry or two entries? Is this per DS or per DS instance?
  message DataSource {
    optional uint32 producer_id = 1;

    # The full descriptor as provided by the producer.
    # Includes name, track_event_descriptor, gpu_counter_descriptor, etc.
    optional DataSourceDescriptor descriptor = 2;

    # How many instances for this data source are active in the current session.
    optional uint32 instances_in_current_session = 3;

    # Same semantic of the fields in Producer.
    optional int64 connect_time_ns = 4;
    optional int64 disconnect_time_ns = 5;
  }
  repeated DataSource data_sources = 3;

  message Sequence {
   # The trusted_sequence_id reported in each TracePacket.
   optional uint32 id = 1;
   optional producer_id = 2;

   # Stats about writes for each sequence (regardless of ring buffer wrapping).
   optional int64 bytes_written = 3;
   optional int64 packets_written = 4;

   # TODO(primiano): this might be possible to track as part of TraceBufferV2.
   optional int64 data_losses = 5;
  }

  # Describes a trace buffer and all the sequences writing to it.
  message Buffer {
   repeated Sequence sequences = 1;
  }
  repeated Buffer buffers = 4;
}
```

## Open questions

#### Should we report all Producers & DataSources, or only the producers that are active in the current trace?

* Why not: we should not pollute the trace with irrelevant info.
* Why yes: in some cases the knowledge that some producers existed at all can help debugging (e.g. selinux, typos, etc)

Overall, I'd say yes, we should pay the cost of reporting all of them (and in a boolean say whether it's involved in the current session)

#### How to model this in TraceProcessor (and in the UI)

Let's leave this for a dedicated RFC.

NOTE: as part of this work we should probably kill TraceStats.WriterStats histograms.
They were added for a specific problem but never used in practice.
