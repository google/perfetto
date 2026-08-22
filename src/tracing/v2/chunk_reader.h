/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACING_V2_CHUNK_READER_H_
#define SRC_TRACING_V2_CHUNK_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/tracing/v2/shared_ring_buffer.h"

namespace perfetto {
namespace tracing_v2 {

// Single-consumer chunk decoder. It reassembles explicitly-sized fragments by
// WriterID, converts nested groups to length-delimited proto, and emits one
// complete canonical TracePacket at a time. It runs in the producer process
// initially; the same bounded validation and reassembly belongs on the
// service side when the ring becomes cross-process.
class ChunkReader {
 public:
  struct Packet {
    WriterID writer_id = 0;
    BufferID target_buffer = 0;
    // TracePacket.previous_packet_dropped reasons, ORed. A bitmask rather than
    // a bool so the specific reason survives: DATA_LOSS_PRESENT alone tells a
    // reader that something was lost but not what, and the field is defined to
    // be composed, not overwritten.
    uint32_t previous_packet_dropped = 0;
    // Valid only for the duration of the callback.
    const uint8_t* data = nullptr;
    size_t size = 0;
  };

  struct DrainResult {
    uint32_t chunks_processed = 0;
    uint32_t packets_read = 0;
    bool has_more = false;
  };

  struct Stats {
    // TODO(sashwinbalaji): export these together with SharedRingBuffer::Stats
    // before enabling the path outside controlled validation.
    uint64_t packets_read = 0;
    uint64_t packets_lost = 0;
    uint64_t malformed_chunks = 0;
    uint64_t orphan_fragments = 0;
  };

  // Bounds one packet, not the sum of partial packets across writers.
  // TODO(sashwinbalaji): add an aggregate incomplete-packet memory budget when
  // this reader moves to the service trust boundary.
  static constexpr size_t kDefaultMaxPacketSize = 64 * 1024 * 1024;

  explicit ChunkReader(SharedRingBuffer*,
                       size_t max_packet_size = kDefaultMaxPacketSize);
  ~ChunkReader() = default;

  ChunkReader(const ChunkReader&) = delete;
  ChunkReader& operator=(const ChunkReader&) = delete;

  // Processes at most one complete ring lap, preventing a continuously active
  // producer from monopolizing the relay task runner. |chunks_processed|
  // counts FIFO positions consumed, including invalidated or malformed chunks.
  DrainResult Drain(const std::function<void(const Packet&)>&);

  // Called only after the ring has reached the writer's retirement watermark.
  // No later chunk can then complete its partial packet, so any remaining
  // fragment is necessarily orphaned and is discarded.
  void ForgetWriter(WriterID);

  const Stats& stats() const { return stats_; }

 private:
  struct WriterState {
    // Packet bytes accumulated across a continuation chain. State is isolated
    // by WriterID so fragments from concurrent packet sequences never mix.
    std::vector<uint8_t> pending_fragment;
    // All fragments of a packet must retain the same routing destination.
    BufferID target_buffer = 0;
    bool has_pending_fragment = false;
    // Loss reasons are attached to the next successfully emitted packet.
    uint32_t pending_loss = 0;
  };

  bool AppendPending(WriterState*, const uint8_t* data, size_t size);
  bool EmitPacket(WriterID,
                  BufferID,
                  WriterState*,
                  const uint8_t* data,
                  size_t size,
                  const std::function<void(const Packet&)>&);
  // |payload_size| is the byte span of the header's num_fragments records,
  // walked and validated by SharedRingBuffer::TryReadChunk() when it copied
  // them into |payload_|.
  uint32_t ProcessChunk(const ChunkHeader&,
                        uint32_t payload_size,
                        const std::function<void(const Packet&)>&);
  void MarkLoss(WriterState*, uint32_t reasons);

  SharedRingBuffer* const ring_;
  const size_t max_packet_size_;
  base::FlatHashMap<WriterID, WriterState> writer_states_;
  std::vector<uint8_t> canonical_packet_;
  uint8_t payload_[kChunkPayloadSize]{};
  Stats stats_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_CHUNK_READER_H_
