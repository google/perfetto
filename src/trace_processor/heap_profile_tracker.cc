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

void HeapProfileTracker::AddAllocation(
    const SourceAllocation& alloc,
    const StackProfileTracker::InternLookup* intern_lookup) {
  auto maybe_callstack_id = context_->stack_profile_tracker->FindCallstack(
      alloc.callstack_id, intern_lookup);
  if (!maybe_callstack_id)
    return;

  int64_t callstack_id = *maybe_callstack_id;

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(alloc.pid));

  TraceStorage::HeapProfileAllocations::Row alloc_row{
      alloc.timestamp, upid, callstack_id,
      static_cast<int64_t>(alloc.alloc_count),
      static_cast<int64_t>(alloc.self_allocated)};

  TraceStorage::HeapProfileAllocations::Row free_row{
      alloc.timestamp, upid, callstack_id,
      -static_cast<int64_t>(alloc.free_count),
      -static_cast<int64_t>(alloc.self_freed)};

  TraceStorage::HeapProfileAllocations::Row alloc_delta = alloc_row;
  TraceStorage::HeapProfileAllocations::Row free_delta = free_row;

  auto prev_alloc_it = prev_alloc_.find({upid, callstack_id});
  if (prev_alloc_it == prev_alloc_.end()) {
    std::tie(prev_alloc_it, std::ignore) =
        prev_alloc_.emplace(std::make_pair(upid, callstack_id),
                            TraceStorage::HeapProfileAllocations::Row{});
  }

  TraceStorage::HeapProfileAllocations::Row& prev_alloc = prev_alloc_it->second;
  alloc_delta.count -= prev_alloc.count;
  alloc_delta.size -= prev_alloc.size;

  auto prev_free_it = prev_free_.find({upid, callstack_id});
  if (prev_free_it == prev_free_.end()) {
    std::tie(prev_free_it, std::ignore) =
        prev_free_.emplace(std::make_pair(upid, callstack_id),
                           TraceStorage::HeapProfileAllocations::Row{});
  }

  TraceStorage::HeapProfileAllocations::Row& prev_free = prev_free_it->second;
  free_delta.count -= prev_free.count;
  free_delta.size -= prev_free.size;

  if (alloc_delta.count)
    context_->storage->mutable_heap_profile_allocations()->Insert(alloc_delta);
  if (free_delta.count)
    context_->storage->mutable_heap_profile_allocations()->Insert(free_delta);

  prev_alloc = alloc_row;
  prev_free = free_row;
}

void HeapProfileTracker::StoreAllocation(SourceAllocation alloc) {
  pending_allocs_.emplace_back(std::move(alloc));
}

void HeapProfileTracker::CommitAllocations(
    const StackProfileTracker::InternLookup* intern_lookup) {
  for (const auto& p : pending_allocs_)
    AddAllocation(p, intern_lookup);
  pending_allocs_.clear();
}

void HeapProfileTracker::FinalizeProfile(
    const StackProfileTracker::InternLookup* intern_lookup) {
  CommitAllocations(intern_lookup);
  context_->stack_profile_tracker->ClearIndices();
}

}  // namespace trace_processor
}  // namespace perfetto
