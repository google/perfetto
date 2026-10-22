/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACING_SERVICE_TRACE_BUFFER_H_
#define SRC_TRACING_SERVICE_TRACE_BUFFER_H_

#include <stdint.h>
#include <array>
#include <memory>
#include <new>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "src/tracing/service/histogram.h"

namespace protozero {
struct ConstBytes;
}  // namespace protozero

namespace perfetto {

class TracePacket;
class TraceBufferProtoVms;
class TraceBuffer_WriterStats;

// Virtual interface for trace buffers to enable multiple implementations.
// This interface defines the minimal surface used by the tracing service.
class TraceBuffer {
 public:
  using WriterStats = TraceBuffer_WriterStats;

  // See comment in the header above.
  enum OverwritePolicy { kOverwrite, kDiscard };

  // Type of trace buffer implementation.
  // TODO(primiano): Remove this once one implementation remains.
  // kV2 and kV3 are experimental.
  enum BufType {
    kV1,
    kV2,
    kV3,
  };

  // Argument for out-of-band patches applied through TryPatchChunkContents().
  struct Patch {
    // From SharedMemoryABI::kPacketHeaderSize.
    static constexpr size_t kSize = 4;

    size_t offset_untrusted;
    std::array<uint8_t, kSize> data;
  };

  // Identifiers that are constant for a packet sequence.
  struct PacketSequenceProperties {
    ProducerID producer_id_trusted;
    ClientIdentity client_identity_trusted;
    WriterID writer_id;

    uid_t producer_uid_trusted() const { return client_identity_trusted.uid(); }
    pid_t producer_pid_trusted() const { return client_identity_trusted.pid(); }
  };

  // Creates the requested implementation and never falls back to another type.
  // Returns an error if the build lacks the implementation (kV3 needs zstd)
  // or allocation fails.
  static base::StatusOr<std::unique_ptr<TraceBuffer>>
  Create(BufType, size_t size_in_bytes, OverwritePolicy);

  virtual ~TraceBuffer();

  // Copies a Chunk from a producer Shared Memory Buffer into the trace buffer.
  virtual void CopyChunkUntrusted(ProducerID producer_id_trusted,
                                  const ClientIdentity& client_identity_trusted,
                                  WriterID writer_id,
                                  ChunkID chunk_id,
                                  uint16_t num_fragments,
                                  uint8_t chunk_flags,
                                  bool chunk_complete,
                                  const uint8_t* src,
                                  size_t size) = 0;

  // Applies a batch of |patches| to the given chunk, if the given chunk is
  // still in the buffer. Does nothing if the given ChunkID is gone.
  // Returns true if the chunk has been found and patched, false otherwise.
  virtual bool TryPatchChunkContents(ProducerID,
                                     WriterID,
                                     ChunkID,
                                     const Patch* patches,
                                     size_t patches_size,
                                     bool other_patches_pending) = 0;

  // Returns true if the buffer accepts tracing v2 data through
  // CopyChunkV2Untrusted(). The service offers tracing v2 only to data sources
  // that write into such a buffer.
  virtual bool supports_tracing_v2() const = 0;

  // Copies a batch of SMB v2 fragments into the buffer. The caller must pass
  // fragments and batches in write order for each {ProducerID, WriterID}.
  // The caller owns the fragment array and payloads. Both must remain valid
  // and unchanged until this call returns.
  //
  // Each fragment is a whole packet unless a continuation flag is set.
  // - |first_continues_from_prev| means the first fragment continues a packet
  //   that started in this writer's previous batch.
  // - |last_continues_on_next| means the last fragment continues in this
  //   writer's next batch.
  // The caller decodes these flags from its transport. If the transport marks
  // a batch as lossy, call RecordChunkV2DataLoss() instead of copying it.
  //
  // Returns true if the whole batch was stored. Returns false if the batch
  // was rejected:
  // - The rejection of a nonempty batch records loss as RecordChunkV2DataLoss()
  //   does. An empty batch changes nothing.
  // - Invalid input increments abi_violations. Other rejections increment
  //   chunks_discarded.
  //
  // If supports_tracing_v2() is false, returns false and changes nothing.
  virtual bool CopyChunkV2Untrusted(const PacketSequenceProperties&,
                                    const protozero::ConstBytes* fragments,
                                    size_t num_fragments,
                                    bool first_continues_from_prev,
                                    bool last_continues_on_next) = 0;

  // Records a gap after the last SMB v2 batch that the writer appended.
  // Packets already in the buffer remain readable. The gap stays pending
  // until the next stored batch carries it or the writer's state is discarded.
  // Reads report the gap on the first readable packet after it. This does not
  // increment any statistics.
  //
  // Does nothing if the buffer holds no SMB v2 state for the writer. A caller
  // that cannot identify the destination can call this on every candidate
  // buffer without creating state for writers that never used it.
  virtual void RecordChunkV2DataLoss(ProducerID, WriterID) = 0;

  // Increments abi_violations for a violation that the caller found outside
  // this buffer, e.g. a protocol error in a producer's ring buffer. This only
  // updates the statistic and does not record a gap on any packet sequence.
  virtual void RecordAbiViolation() = 0;

  // Call this after copying a complete write batch, including its patches.
  // A CommitData request, an SMB v2 drain pass and an SMB v1 scrape each end
  // a batch.
  // Chunks can arrive out of ChunkID order within a batch.
  // This call lets the buffer read past gaps that the rest of the batch did
  // not fill.
  // A batch can end in the middle of a packet.
  // The checkpoint can read packets from every producer, not only from the
  // one that ended the batch.
  // It does not have to read every readable packet.
  // TraceBufferV3 bundles staged packets here once staging has stored enough
  // new data.
  //
  // The default implementation does nothing.
  virtual void OnWriteCheckpoint();

  // To read the contents of the buffer the caller needs to:
  //   BeginRead()
  //   while (ReadNextTracePacket(packet_fragments)) { ... }
  // No other calls to any other method should be interleaved between
  // BeginRead() and ReadNextTracePacket().
  // Release packets from the previous read before calling BeginRead() again.
  // TraceBufferV3 reuses the decode memory behind their slices.
  // Reads in the TraceBuffer are NOT idempotent.
  virtual void BeginRead() = 0;

  // Returns the next packet in the buffer, if any, and the producer_id,
  // producer_uid, and writer_id of the producer/writer that wrote it.
  // Returns false if no packets can be read at this point.
  // |previous_packet_on_sequence_dropped| is a bitmask: 0 if no data was lost
  // on the sequence before this packet, otherwise nonzero.
  // TraceBufferV2 and TraceBufferV3 set DataLossReason bits.
  // TraceBufferV1 sets 1.
  // The returned bytes have not passed PacketStreamValidator.
  // The service validates the packet first, then appends a nonzero value as
  // TracePacket.previous_packet_dropped.
  virtual bool ReadNextTracePacket(
      TracePacket*,
      PacketSequenceProperties* sequence_properties,
      uint32_t* previous_packet_on_sequence_dropped) = 0;

  // Attempts to reclaim unused buffer space.
  // The caller must release all packets from previous reads before this call,
  // because their slices can point into memory this call moves or frees.
  // Call BeginRead() before reading again.
  virtual void MaybeCompact() = 0;

  // Creates a read-only clone of the trace buffer.
  // Call BeginRead() before reading the clone.
  // Consumed packets stay consumed, except in TraceBufferV1 when
  // BUFFER_CLONE_PRESERVE_READ_ITER is disabled.
  virtual std::unique_ptr<TraceBuffer> CloneReadOnly() const = 0;

  // Returns the ProtoVM state owned by this buffer, or nullptr if the
  // implementation cannot run a ProtoVM. The pointer remains valid until the
  // buffer is destroyed.
  // The service uses it to set up VMs and emit their state. The buffer feeds
  // packets to the VMs before overwriting them.
  virtual TraceBufferProtoVms* protovms() = 0;

  virtual void set_read_only() = 0;
  virtual const TraceStats::BufferStats& stats() const = 0;
  virtual const WriterStats& writer_stats() const = 0;
  virtual size_t size() const = 0;
  virtual size_t used_size() const = 0;
  virtual size_t GetMemoryUsageBytes() const = 0;
  virtual OverwritePolicy overwrite_policy() const = 0;
  virtual bool has_data() const = 0;
  virtual BufType buf_type() const = 0;

  // Exposed for test/fake_packet.{cc,h}.
  static inline constexpr size_t InlineChunkHeaderSize = 16;
};

class TraceBuffer_WriterStats {
 public:
  using WriterBuckets =
      Histogram<8, 32, 128, 512, 1024, 2048, 4096, 8192, 12288, 16384>;
  struct ChunkUsage {
    WriterBuckets chunk_payload_histogram;
    // ProtocolAbiVersion values that carried this writer's chunks.
    uint32_t protocol_abi_versions = 0;
  };
  using WriterStatsMap = base::FlatHashMap<ProducerAndWriterID,
                                           ChunkUsage,
                                           std::hash<ProducerAndWriterID>,
                                           base::QuadraticProbe,
                                           /*AppendOnly=*/true>;

  TraceBuffer_WriterStats() = default;
  TraceBuffer_WriterStats(const TraceBuffer_WriterStats& other) {
    for (auto it = other.map_.GetIterator(); it; ++it)
      map_.Insert(it.key(), it.value());
  }
  TraceBuffer_WriterStats& operator=(const TraceBuffer_WriterStats& other) {
    if (this != &other) {
      this->~TraceBuffer_WriterStats();
      new (this) TraceBuffer_WriterStats(other);
    }
    return *this;
  }

  // Adds one chunk of payload size |val|.
  // |protocol_abi_version| is the ProtocolAbiVersion that carried it.
  void Insert(ProducerAndWriterID key,
              HistValue val,
              uint32_t protocol_abi_version) {
    ChunkUsage& usage = *map_.Insert(key, {}).first;
    usage.protocol_abi_versions |= protocol_abi_version;
    usage.chunk_payload_histogram.Add(val);
  }

  WriterStatsMap::Iterator GetIterator() const { return map_.GetIterator(); }

 private:
  WriterStatsMap map_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_H_
