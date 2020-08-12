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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_PROFILE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_PROFILE_TRACKER_H_

#include <set>
#include <unordered_map>

#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/importers/proto/stack_profile_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
BuildNativeFlamegraph(TraceStorage* storage, UniquePid upid, int64_t timestamp);

class TraceProcessorContext;

class HeapProfileTracker {
 public:
  struct SourceAllocation {
    uint64_t pid = 0;
    // This is int64_t, because we get this from the TraceSorter which also
    // converts this for us.
    int64_t timestamp = 0;
    StringPool::Id heap_name;
    SequenceStackProfileTracker::SourceCallstackId callstack_id = 0;
    uint64_t self_allocated = 0;
    uint64_t self_freed = 0;
    uint64_t alloc_count = 0;
    uint64_t free_count = 0;
  };

  void SetProfilePacketIndex(uint32_t seq_id, uint64_t id);

  explicit HeapProfileTracker(TraceProcessorContext* context);

  void StoreAllocation(uint32_t seq_id, SourceAllocation);

  // Call after the last profile packet of a dump to commit the allocations
  // that had been stored using StoreAllocation and clear internal indices
  // for that dump.
  void FinalizeProfile(
      uint32_t seq_id,
      SequenceStackProfileTracker* sequence_stack_profile_tracker,
      const SequenceStackProfileTracker::InternLookup* lookup);

  // Only commit the allocations that had been stored using StoreAllocations.
  // This is only needed in tests, use FinalizeProfile instead.
  void CommitAllocations(
      uint32_t seq_id,
      SequenceStackProfileTracker* sequence_stack_profile_tracker,
      const SequenceStackProfileTracker::InternLookup* lookup);

  void NotifyEndOfFile();

  ~HeapProfileTracker();

 private:
  void AddAllocation(
      uint32_t seq_id,
      SequenceStackProfileTracker* sequence_stack_profile_tracker,
      const SourceAllocation&,
      const SequenceStackProfileTracker::InternLookup* intern_lookup = nullptr);
  struct SourceAllocationIndex {
    UniquePid upid;
    SequenceStackProfileTracker::SourceCallstackId src_callstack_id;
    StringPool::Id heap_name;
    bool operator<(const SourceAllocationIndex& o) const {
      return std::tie(upid, src_callstack_id, heap_name) <
             std::tie(o.upid, o.src_callstack_id, o.heap_name);
    }
  };
  struct SequenceState {
    std::vector<SourceAllocation> pending_allocs;

    std::unordered_map<std::pair<UniquePid, CallsiteId>,
                       tables::HeapProfileAllocationTable::Row>
        prev_alloc;
    std::unordered_map<std::pair<UniquePid, CallsiteId>,
                       tables::HeapProfileAllocationTable::Row>
        prev_free;

    // For continuous dumps, we only store the delta in the data-base. To do
    // this, we subtract the previous dump's value. Sometimes, we should not
    // do that subtraction, because heapprofd garbage collects stacks that
    // have no unfreed allocations. If the application then allocations again
    // at that stack, it gets recreated and initialized to zero.
    //
    // To correct for this, we add the previous' stacks value to the current
    // one, and then handle it as normal. If it is the first time we see a
    // SourceCallstackId for a CallsiteId, we put the previous value into
    // the correction maps below.
    std::map<SourceAllocationIndex, std::set<CallsiteId>> seen_callstacks;
    std::map<SequenceStackProfileTracker::SourceCallstackId,
             tables::HeapProfileAllocationTable::Row>
        alloc_correction;
    std::map<SequenceStackProfileTracker::SourceCallstackId,
             tables::HeapProfileAllocationTable::Row>
        free_correction;

    base::Optional<uint64_t> prev_index;
  };
  std::map<uint32_t, SequenceState> sequence_state_;
  TraceProcessorContext* const context_;
  const StringId empty_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_PROFILE_TRACKER_H_
