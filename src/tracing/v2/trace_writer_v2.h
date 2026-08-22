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

#ifndef SRC_TRACING_V2_TRACE_WRITER_V2_H_
#define SRC_TRACING_V2_TRACE_WRITER_V2_H_

#include <stdint.h>

#include <array>
#include <functional>
#include <memory>

#include "perfetto/base/proc_utils.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"

namespace perfetto {
namespace tracing_v2 {

class TraceWriterV2 : public TraceWriter,
                      public protozero::MessageFinalizationListener,
                      public protozero::ScatteredStreamWriter::Delegate {
 public:
  class Delegate {
   public:
    virtual ~Delegate();
    // These notifications are part of the writer lifecycle. Their final
    // transport can change, but packet commits, flush watermarks and writer
    // retirement all need an acknowledgement path after the temporary bridge is
    // removed.
    virtual void OnPacketsCommitted() = 0;
    virtual void OnWriterFlush(WriterID,
                               uint32_t pos,
                               std::function<void()>) = 0;
    virtual void OnWriterDestroyed(WriterID, uint32_t pos) = 0;
  };

  struct InitArgs {
    SharedRingBuffer* ring_buffer = nullptr;
    std::shared_ptr<Delegate> delegate;
    // These are borrowed from the downstream v1 writer. The direct v2 path
    // will obtain writer identity and routing from producer/service
    // negotiation instead.
    WriterID writer_id = 0;
    BufferID target_buffer = 0;
    BufferExhaustedPolicy buffer_exhausted_policy =
        BufferExhaustedPolicy::kDrop;

    // Overridden in tests so the stall paths take milliseconds, not seconds.
    // How long a kStallThenDrop writer waits before giving up and dropping.
    uint32_t stall_then_drop_timeout_ms = 2000;
    // A kStall writer blocked this long means the relay is wedged or the
    // process has deadlocked against itself. v1 aborts in the same situation
    // (kAssertAtNStalls); so do we, because silently tracing nothing is worse.
    uint32_t stall_fatal_timeout_ms = 30000;
  };

  explicit TraceWriterV2(const InitArgs&);
  ~TraceWriterV2() override;

  TraceWriterV2(const TraceWriterV2&) = delete;
  TraceWriterV2& operator=(const TraceWriterV2&) = delete;
  TraceWriterV2(TraceWriterV2&&) = delete;
  TraceWriterV2& operator=(TraceWriterV2&&) = delete;

  TracePacketHandle NewTracePacket() override;
  void FinishTracePacket() override;
  void Flush(std::function<void()> callback = {}) override;
  WriterID writer_id() const override;
  uint64_t written() const override;
  uint64_t drop_count() const override;

 private:
  protozero::ContiguousMemoryRange GetNewBuffer() override;
  uint8_t* AnnotatePatch(uint8_t*) override;
  void OnMessageFinalized(protozero::Message*) override;

  bool TryAcquireChunk(uint8_t flags);
  // Blocks until the reader frees a chunk. Returns false when the policy says
  // to give up instead: immediately for kDrop, after a deadline for
  // kStallThenDrop. kStall never returns false, it aborts.
  bool StallForChunk(uint32_t reader_generation, int64_t stall_started_ms);
  protozero::ContiguousMemoryRange WritableChunkRange() const;
  protozero::ContiguousMemoryRange EnterDropMode();
  void StartFragment();
  void FinalizeFragment();
  void PublishCurrentChunk(uint8_t added_flags);

  SharedRingBuffer* const ring_buffer_;
  const std::shared_ptr<Delegate> delegate_;
  const WriterID writer_id_;
  const BufferID target_buffer_;
  const BufferExhaustedPolicy buffer_exhausted_policy_;
  const uint32_t stall_then_drop_timeout_ms_;
  const uint32_t stall_fatal_timeout_ms_;

  SharedRingBuffer::Chunk current_chunk_;
  protozero::ScatteredStreamWriter protobuf_stream_writer_;
  std::unique_ptr<protozero::RootMessage<protos::pbzero::TracePacket>>
      current_packet_;
  std::array<uint8_t, kChunkPayloadSize> garbage_buffer_{};

  uint8_t* fragment_size_field_ = nullptr;
  uint8_t* fragment_begin_ = nullptr;
  uint64_t drop_count_ = 0;
  bool packet_open_ = false;
  bool pending_data_loss_ = false;
  bool drop_packets_ = false;

  const base::PlatformProcessId process_id_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_TRACE_WRITER_V2_H_
