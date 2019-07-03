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

#ifndef SRC_TRACE_PROCESSOR_HEAP_PROFILE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_HEAP_PROFILE_TRACKER_H_

#include <deque>

#include "perfetto/ext/base/optional.h"

#include "perfetto/trace/profiling/profile_common.pbzero.h"
#include "perfetto/trace/profiling/profile_packet.pbzero.h"
#include "src/trace_processor/trace_storage.h"

namespace std {

template <>
struct hash<std::pair<uint32_t, int64_t>> {
  using argument_type = std::pair<uint32_t, int64_t>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    return std::hash<uint32_t>{}(p.first) ^ std::hash<int64_t>{}(p.second);
  }
};

template <>
struct hash<std::vector<uint64_t>> {
  using argument_type = std::vector<uint64_t>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    size_t h = 0u;
    for (auto v : p)
      h = h ^ std::hash<uint64_t>{}(v);
    return h;
  }
};

}  // namespace std
namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class HeapProfileTracker {
 public:
  using SourceStringId = uint64_t;

  struct SourceMapping {
    SourceStringId build_id = 0;
    uint64_t exact_offset = 0;
    uint64_t start_offset = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t load_bias = 0;
    SourceStringId name_id = 0;
  };
  using SourceMappingId = uint64_t;

  struct SourceFrame {
    SourceStringId name_id = 0;
    SourceMappingId mapping_id = 0;
    uint64_t rel_pc = 0;
  };
  using SourceFrameId = uint64_t;

  using SourceCallstack = std::vector<SourceFrameId>;
  using SourceCallstackId = uint64_t;

  struct SourceAllocation {
    uint64_t pid = 0;
    // This is int64_t, because we get this from the TraceSorter which also
    // converts this for us.
    int64_t timestamp = 0;
    SourceCallstackId callstack_id = 0;
    uint64_t self_allocated = 0;
    uint64_t self_freed = 0;
    uint64_t alloc_count = 0;
    uint64_t free_count = 0;
  };

  class InternLookup {
   public:
    virtual ~InternLookup();

    virtual base::Optional<StringId> GetString(SourceStringId) const = 0;
    virtual base::Optional<SourceMapping> GetMapping(SourceMappingId) const = 0;
    virtual base::Optional<SourceFrame> GetFrame(SourceFrameId) const = 0;
    virtual base::Optional<SourceCallstack> GetCallstack(
        SourceCallstackId) const = 0;
  };

  explicit HeapProfileTracker(TraceProcessorContext* context);

  void AddString(SourceStringId, StringId);
  int64_t AddMapping(SourceMappingId,
                     const SourceMapping&,
                     const InternLookup* intern_lookup = nullptr);
  int64_t AddFrame(SourceFrameId,
                   const SourceFrame&,
                   const InternLookup* intern_lookup = nullptr);
  int64_t AddCallstack(SourceCallstackId,
                       const SourceCallstack&,
                       const InternLookup* intern_lookup = nullptr);

  void StoreAllocation(SourceAllocation);
  // Call after the last profile packet of a dump to commit the allocations
  // that had been stored using StoreAllocation and clear internal indices
  // for that dump.
  void FinalizeProfile(const InternLookup* lookup);
  // Only commit the allocations that had been stored using StoreAllocations.
  // This is only needed in tests, use FinalizeProfile instead.
  void CommitAllocations(const InternLookup* lookup);
  int64_t GetDatabaseFrameIdForTesting(SourceFrameId);

  ~HeapProfileTracker();

 private:
  void AddAllocation(const SourceAllocation&,
                     const InternLookup* intern_lookup = nullptr);

  // Gets the row number of string / mapping / frame / callstack previously
  // added through AddString / AddMapping/ AddFrame / AddCallstack.
  //
  // If it is not found, look up the string / mapping / frame / callstack in
  // the global InternedData state, and if found, add to the database, if not
  // already added before.
  //
  // This is to support both ProfilePackets that contain the interned data
  // (for Android Q) and where the interned data is kept globally in
  // InternedData (for versions newer than Q).
  base::Optional<StringId> FindString(SourceStringId,
                                      const InternLookup* intern_lookup);
  base::Optional<int64_t> FindMapping(SourceMappingId,
                                      const InternLookup* intern_lookup);
  base::Optional<int64_t> FindFrame(SourceFrameId,
                                    const InternLookup* intern_lookup);
  base::Optional<int64_t> FindCallstack(SourceCallstackId,
                                        const InternLookup* intern_lookup);

  std::unordered_map<SourceStringId, StringId> string_map_;
  std::unordered_map<SourceMappingId, int64_t> mappings_;
  std::unordered_map<SourceFrameId, int64_t> frames_;
  std::unordered_map<SourceCallstack, int64_t> callstacks_from_frames_;
  std::unordered_map<SourceCallstackId, int64_t> callstacks_;
  std::vector<SourceAllocation> pending_allocs_;

  std::unordered_map<TraceStorage::HeapProfileMappings::Row, int64_t>
      mapping_idx_;
  std::unordered_map<TraceStorage::HeapProfileFrames::Row, int64_t> frame_idx_;
  std::unordered_map<TraceStorage::HeapProfileCallsites::Row, int64_t>
      callsite_idx_;

  std::unordered_map<std::pair<UniquePid, int64_t>,
                     TraceStorage::HeapProfileAllocations::Row>
      prev_alloc_;
  std::unordered_map<std::pair<UniquePid, int64_t>,
                     TraceStorage::HeapProfileAllocations::Row>
      prev_free_;

  TraceProcessorContext* const context_;
  const StringId empty_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_HEAP_PROFILE_TRACKER_H_
