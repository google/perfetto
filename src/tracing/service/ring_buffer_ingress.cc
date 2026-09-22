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

#include "src/tracing/service/ring_buffer_ingress.h"

#include <utility>

#include "perfetto/base/logging.h"
#include "src/tracing/service/trace_buffer_v2.h"

namespace perfetto::tracing_v2 {

RingBufferIngress::Delegate::~Delegate() = default;

RingBufferIngress::RingBufferIngress(std::unique_ptr<SharedMemory> memory,
                                     uint32_t chunk_size_bytes,
                                     ProducerID producer_id,
                                     ClientIdentity client_identity,
                                     Delegate* delegate,
                                     base::TaskRunner* task_runner)
    : producer_id_(producer_id),
      client_identity_(client_identity),
      delegate_(delegate),
      memory_(std::move(memory)),
      ring_buffer_(static_cast<uint8_t*>(memory_->start()),
                   memory_->size(),
                   chunk_size_bytes),
      reader_(&ring_buffer_, this),
      weak_runner_(task_runner) {}

RingBufferIngress::~RingBufferIngress() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
}

void RingBufferIngress::Drain() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (reader_.has_protocol_error())
    return;
  const uint32_t count =
      ring_buffer_.LoadWritePosRelaxed() - reader_.read_pos();
  auto result = reader_.Drain(count);
  if (!reader_.has_protocol_error() && result.positions_consumed != count &&
      !retry_scheduled_) {
    retry_scheduled_ = true;
    weak_runner_.PostTask([this] {
      retry_scheduled_ = false;
      Drain();
    });
  }
}

void RingBufferIngress::OnChunkRead(
    const SharedRingBufferReader::ChunkContents& chunk) {
  if (!chunk.writer_id || chunk.writer_id > kMaxWriterID) {
    delegate_->OnRingBufferChunkDiscarded();
    return;
  }
  TraceBufferV2* buffer =
      delegate_->GetRingBufferDestination(chunk.target_buffer);
  if (!buffer) {
    OnDataLoss(chunk.writer_id);
    return;
  }
  TraceBufferV2::ProtoGroupSequence sequence{producer_id_, client_identity_,
                                             chunk.writer_id};
  if (buffer->AppendProtoGroupFragments(sequence, chunk.fragments,
                                        chunk.num_fragments,
                                        chunk.payload_flags) !=
      TraceBufferV2::ProtoGroupAppendResult::kStored) {
    delegate_->OnRingBufferChunkDiscarded();
  }
}

void RingBufferIngress::OnDataLoss(WriterID writer_id) {
  delegate_->OnRingBufferChunkDiscarded();
  if (!writer_id || writer_id > kMaxWriterID)
    return;
  TraceBufferV2::ProtoGroupSequence sequence{producer_id_, client_identity_,
                                             writer_id};
  // A writer has one destination, so normally at most one sequence matches.
  // Before its first admitted chunk, no sequence matches. The statistic above
  // still counts that loss without separate state for each writer.
  delegate_->ForEachRingBufferDestination([&sequence](TraceBufferV2& buffer) {
    if (buffer.HasSequence(sequence.producer_id_trusted, sequence.writer_id))
      buffer.RecordProtoGroupLoss(sequence);
  });
}

}  // namespace perfetto::tracing_v2
