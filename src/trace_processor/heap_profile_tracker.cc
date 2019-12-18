/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/heap_profile_tracker.h"

#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

HeapProfileTracker::HeapProfileTracker(TraceProcessorContext* context)
    : context_(context), empty_(context_->storage->InternString({"", 0})) {}

HeapProfileTracker::~HeapProfileTracker() = default;

void HeapProfileTracker::SetProfilePacketIndex(uint32_t seq_id,
                                               uint64_t index) {
  SequenceState& sequence_state = sequence_state_[seq_id];
  if (sequence_state.last_profile_packet_index != 0 &&
      sequence_state.last_profile_packet_index + 1 != index) {
    context_->storage->IncrementStats(stats::heapprofd_missing_packet);
  }
  sequence_state.last_profile_packet_index = index;
}

void HeapProfileTracker::AddAllocation(
    uint32_t seq_id,
    StackProfileTracker* stack_profile_tracker,
    const SourceAllocation& alloc,
    const StackProfileTracker::InternLookup* intern_lookup) {
  SequenceState& sequence_state = sequence_state_[seq_id];
  auto maybe_callstack_id =
      stack_profile_tracker->FindCallstack(alloc.callstack_id, intern_lookup);
  if (!maybe_callstack_id)
    return;

  int64_t callstack_id = *maybe_callstack_id;

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(alloc.pid));

  tables::HeapProfileAllocationTable::Row alloc_row{
      alloc.timestamp, upid, callstack_id,
      static_cast<int64_t>(alloc.alloc_count),
      static_cast<int64_t>(alloc.self_allocated)};

  tables::HeapProfileAllocationTable::Row free_row{
      alloc.timestamp, upid, callstack_id,
      -static_cast<int64_t>(alloc.free_count),
      -static_cast<int64_t>(alloc.self_freed)};

  tables::HeapProfileAllocationTable::Row alloc_delta = alloc_row;
  tables::HeapProfileAllocationTable::Row free_delta = free_row;

  auto prev_alloc_it = sequence_state.prev_alloc.find({upid, callstack_id});
  if (prev_alloc_it == sequence_state.prev_alloc.end()) {
    std::tie(prev_alloc_it, std::ignore) = sequence_state.prev_alloc.emplace(
        std::make_pair(upid, callstack_id),
        tables::HeapProfileAllocationTable::Row{});
  }

  tables::HeapProfileAllocationTable::Row& prev_alloc = prev_alloc_it->second;
  alloc_delta.count -= prev_alloc.count;
  alloc_delta.size -= prev_alloc.size;

  auto prev_free_it = sequence_state.prev_free.find({upid, callstack_id});
  if (prev_free_it == sequence_state.prev_free.end()) {
    std::tie(prev_free_it, std::ignore) = sequence_state.prev_free.emplace(
        std::make_pair(upid, callstack_id),
        tables::HeapProfileAllocationTable::Row{});
  }

  tables::HeapProfileAllocationTable::Row& prev_free = prev_free_it->second;
  free_delta.count -= prev_free.count;
  free_delta.size -= prev_free.size;

  if (alloc_delta.count)
    context_->storage->mutable_heap_profile_allocation_table()->Insert(
        alloc_delta);
  if (free_delta.count)
    context_->storage->mutable_heap_profile_allocation_table()->Insert(
        free_delta);

  prev_alloc = alloc_row;
  prev_free = free_row;
}

void HeapProfileTracker::StoreAllocation(uint32_t seq_id,
                                         SourceAllocation alloc) {
  SequenceState& sequence_state = sequence_state_[seq_id];
  sequence_state.pending_allocs.emplace_back(std::move(alloc));
}

void HeapProfileTracker::CommitAllocations(
    uint32_t seq_id,
    StackProfileTracker* stack_profile_tracker,
    const StackProfileTracker::InternLookup* intern_lookup) {
  SequenceState& sequence_state = sequence_state_[seq_id];
  for (const auto& p : sequence_state.pending_allocs)
    AddAllocation(seq_id, stack_profile_tracker, p, intern_lookup);
  sequence_state.pending_allocs.clear();
}

void HeapProfileTracker::FinalizeProfile(
    uint32_t seq_id,
    StackProfileTracker* stack_profile_tracker,
    const StackProfileTracker::InternLookup* intern_lookup) {
  CommitAllocations(seq_id, stack_profile_tracker, intern_lookup);
  stack_profile_tracker->ClearIndices();
}

}  // namespace trace_processor
}  // namespace perfetto
