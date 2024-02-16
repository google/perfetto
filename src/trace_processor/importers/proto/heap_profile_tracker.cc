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

#include "src/trace_processor/importers/proto/heap_profile_tracker.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

HeapProfileTracker::HeapProfileTracker(TraceProcessorContext* context)
    : context_(context),
      empty_(context_->storage->InternString({"", 0})),
      art_heap_(context_->storage->InternString("com.android.art")) {}

HeapProfileTracker::~HeapProfileTracker() = default;

void HeapProfileTracker::SetProfilePacketIndex(uint32_t seq_id,
                                               uint64_t index) {
  SequenceState& sequence_state = sequence_state_[seq_id];
  bool dropped_packet = false;
  // heapprofd starts counting at index = 0.
  if (!sequence_state.prev_index && index != 0) {
    dropped_packet = true;
  }

  if (sequence_state.prev_index && *sequence_state.prev_index + 1 != index) {
    dropped_packet = true;
  }

  if (dropped_packet) {
    if (sequence_state.prev_index) {
      PERFETTO_ELOG("Missing packets between %" PRIu64 " and %" PRIu64,
                    *sequence_state.prev_index, index);
    } else {
      PERFETTO_ELOG("Invalid first packet index %" PRIu64 " (!= 0)", index);
    }

    context_->storage->IncrementStats(stats::heapprofd_missing_packet);
  }
  sequence_state.prev_index = index;
}

void HeapProfileTracker::AddAllocation(
    uint32_t seq_id,
    SequenceStackProfileTracker* sequence_stack_profile_tracker,
    const SourceAllocation& alloc,
    const SequenceStackProfileTracker::InternLookup* intern_lookup) {
  SequenceState& sequence_state = sequence_state_[seq_id];

  auto opt_callstack_id = sequence_stack_profile_tracker->FindOrInsertCallstack(
      alloc.callstack_id, intern_lookup);
  if (!opt_callstack_id)
    return;

  CallsiteId callstack_id = *opt_callstack_id;

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(alloc.pid));

  tables::HeapProfileAllocationTable::Row alloc_row{
      alloc.timestamp,
      upid,
      alloc.heap_name,
      callstack_id,
      static_cast<int64_t>(alloc.alloc_count),
      static_cast<int64_t>(alloc.self_allocated)};

  tables::HeapProfileAllocationTable::Row free_row{
      alloc.timestamp,
      upid,
      alloc.heap_name,
      callstack_id,
      -static_cast<int64_t>(alloc.free_count),
      -static_cast<int64_t>(alloc.self_freed)};

  auto prev_alloc_it = sequence_state.prev_alloc.find({upid, callstack_id});
  if (prev_alloc_it == sequence_state.prev_alloc.end()) {
    std::tie(prev_alloc_it, std::ignore) = sequence_state.prev_alloc.emplace(
        std::make_pair(upid, callstack_id),
        tables::HeapProfileAllocationTable::Row{});
  }

  tables::HeapProfileAllocationTable::Row& prev_alloc = prev_alloc_it->second;

  auto prev_free_it = sequence_state.prev_free.find({upid, callstack_id});
  if (prev_free_it == sequence_state.prev_free.end()) {
    std::tie(prev_free_it, std::ignore) = sequence_state.prev_free.emplace(
        std::make_pair(upid, callstack_id),
        tables::HeapProfileAllocationTable::Row{});
  }

  tables::HeapProfileAllocationTable::Row& prev_free = prev_free_it->second;

  std::set<CallsiteId>& callstacks_for_source_callstack_id =
      sequence_state.seen_callstacks[SourceAllocationIndex{
          upid, alloc.callstack_id, alloc.heap_name}];
  bool new_callstack;
  std::tie(std::ignore, new_callstack) =
      callstacks_for_source_callstack_id.emplace(callstack_id);

  if (new_callstack) {
    sequence_state.alloc_correction[alloc.callstack_id] = prev_alloc;
    sequence_state.free_correction[alloc.callstack_id] = prev_free;
  }

  auto alloc_correction_it =
      sequence_state.alloc_correction.find(alloc.callstack_id);
  if (alloc_correction_it != sequence_state.alloc_correction.end()) {
    const auto& alloc_correction = alloc_correction_it->second;
    alloc_row.count += alloc_correction.count;
    alloc_row.size += alloc_correction.size;
  }

  auto free_correction_it =
      sequence_state.free_correction.find(alloc.callstack_id);
  if (free_correction_it != sequence_state.free_correction.end()) {
    const auto& free_correction = free_correction_it->second;
    free_row.count += free_correction.count;
    free_row.size += free_correction.size;
  }

  tables::HeapProfileAllocationTable::Row alloc_delta = alloc_row;
  tables::HeapProfileAllocationTable::Row free_delta = free_row;

  alloc_delta.count -= prev_alloc.count;
  alloc_delta.size -= prev_alloc.size;

  free_delta.count -= prev_free.count;
  free_delta.size -= prev_free.size;

  if (alloc_delta.count < 0 || alloc_delta.size < 0 || free_delta.count > 0 ||
      free_delta.size > 0) {
    PERFETTO_DLOG("Non-monotonous allocation.");
    context_->storage->IncrementIndexedStats(stats::heapprofd_malformed_packet,
                                             static_cast<int>(upid));
    return;
  }

  // Dump at max profiles do not have .count set.
  if (alloc_delta.count || alloc_delta.size) {
    context_->storage->mutable_heap_profile_allocation_table()->Insert(
        alloc_delta);
  }

  // ART only reports allocations, and not frees. This throws off our logic
  // that assumes that if a new object was allocated with the same address,
  // the old one has to have been freed in the meantime.
  // See HeapTracker::RecordMalloc in bookkeeping.cc.
  if (alloc.heap_name != art_heap_ && (free_delta.count || free_delta.size)) {
    context_->storage->mutable_heap_profile_allocation_table()->Insert(
        free_delta);
  }

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
    SequenceStackProfileTracker* sequence_stack_profile_tracker,
    const SequenceStackProfileTracker::InternLookup* intern_lookup) {
  SequenceState& sequence_state = sequence_state_[seq_id];
  for (const auto& p : sequence_state.pending_allocs)
    AddAllocation(seq_id, sequence_stack_profile_tracker, p, intern_lookup);
  sequence_state.pending_allocs.clear();
}

void HeapProfileTracker::FinalizeProfile(
    uint32_t seq_id,
    SequenceStackProfileTracker* sequence_stack_profile_tracker,
    const SequenceStackProfileTracker::InternLookup* intern_lookup) {
  CommitAllocations(seq_id, sequence_stack_profile_tracker, intern_lookup);
  sequence_stack_profile_tracker->ClearIndices();
}

void HeapProfileTracker::NotifyEndOfFile() {
  for (const auto& key_and_sequence_state : sequence_state_) {
    const SequenceState& sequence_state = key_and_sequence_state.second;
    if (!sequence_state.pending_allocs.empty()) {
      context_->storage->IncrementStats(stats::heapprofd_non_finalized_profile);
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
