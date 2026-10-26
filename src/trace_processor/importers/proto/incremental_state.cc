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

#include "src/trace_processor/importers/proto/incremental_state.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/interned_message_view.h"

namespace perfetto::trace_processor {

CustomState::~CustomState() = default;

// static
RefPtr<IncrementalState> IncrementalState::CreateSuccessor(
    const IncrementalState& prev) {
  auto incr = RefPtr<IncrementalState>(new IncrementalState(prev.context_));
  incr->thread_descriptor_ = prev.thread_descriptor_;
  incr->copies_ = prev.copies_;
  return incr;
}

// static
RefPtr<IncrementalState> IncrementalState::CreateAfterPacketLoss(
    const IncrementalState& prev) {
  auto incr = RefPtr<IncrementalState>(new IncrementalState(prev.context_));
  for (auto it = prev.interned_data_.GetIterator(); it; ++it)
    incr->interned_data_.Insert(it.key(), it.value());
  incr->thread_descriptor_ = prev.thread_descriptor_;
  incr->copies_ = prev.copies_;
  // Share non-opt-in CustomStates with |prev| (RefPtr copy). Opt-in slots
  // (ClearOnPacketLoss() == true) start empty in the new IS and will be
  // lazy-recreated; the original stays in |prev| for buffered pre-loss
  // packets.
  incr->custom_state_ = prev.custom_state_;
  for (auto& cs : incr->custom_state_) {
    if (cs && cs->ClearOnPacketLoss())
      cs.reset();
  }
  return incr;
}

void IncrementalState::OnInternedMessageMissing() {
  context_->stats_tracker->IncrementStats(
      stats::interned_data_tokenizer_errors);
}

void IncrementalState::InternMessage(uint32_t field_id, TraceBlobView message) {
  constexpr auto kIidFieldNumber = 1;

  uint64_t iid = 0;
  auto message_start = message.data();
  auto message_size = message.length();
  protozero::ProtoDecoder decoder(message_start, message_size);

  auto field = decoder.FindField(kIidFieldNumber);
  if (PERFETTO_UNLIKELY(!field)) {
    PERFETTO_DLOG("Interned message without interning_id");
    context_->stats_tracker->IncrementStats(
        stats::interned_data_tokenizer_errors);
    return;
  }
  iid = field.as_uint64();

  // Re-emitted messages must be identical (the InternedData proto forbids
  // changing them), so only the first copy is kept.
  if (auto* existing = interned_data_.Find({field_id, iid}); existing) {
    PERFETTO_DCHECK(
        existing->message().length() == message_size &&
        memcmp(existing->message().data(), message_start, message_size) == 0);
    return;
  }
  interned_data_.Insert({field_id, iid}, InternedMessageView(copies_->Copy(
                                             {field_id, iid}, message)));
}

TraceBlobView InternedCopies::Copy(InternedDataKey key,
                                   const TraceBlobView& message) {
  static constexpr size_t kArenaChunkSize = 64 * 1024;
  static constexpr size_t kMaxInChunk = kArenaChunkSize / 4;
  size_t size = message.size();
  TraceBlobView* latest = latest_.Find(key);
  if (latest && latest->size() == size &&
      memcmp(latest->data(), message.data(), size) == 0) {
    return latest->copy();
  }
  TraceBlobView view;
  if (size > kMaxInChunk) {
    view = TraceBlobView(TraceBlob::CopyFrom(message.data(), size));
  } else {
    if (!arena_chunk_ || arena_used_ + size > kArenaChunkSize) {
      arena_chunk_.reset(new TraceBlob(TraceBlob::Allocate(kArenaChunkSize)));
      arena_used_ = 0;
    }
    memcpy(arena_chunk_->mutable_data() + arena_used_, message.data(), size);
    view = TraceBlobView(arena_chunk_, arena_used_, size);
    arena_used_ += size;
  }
  if (latest) {
    *latest = view.copy();
  } else {
    latest_.Insert(key, view.copy());
  }
  return view;
}

}  // namespace perfetto::trace_processor
