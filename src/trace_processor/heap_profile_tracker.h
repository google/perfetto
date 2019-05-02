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

#include "perfetto/trace/profiling/profile_packet.pbzero.h"
#include "src/trace_processor/trace_storage.h"

namespace std {

template <>
struct hash<std::pair<uint64_t, uint64_t>> {
  using argument_type = std::pair<uint64_t, uint64_t>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    return std::hash<uint64_t>{}(p.first) ^ std::hash<uint64_t>{}(p.second);
  }
};

template <>
struct hash<std::pair<uint64_t, std::vector<uint64_t>>> {
  using argument_type = std::pair<uint64_t, std::vector<uint64_t>>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    auto h = std::hash<uint64_t>{}(p.first);
    for (auto v : p.second)
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
  // Not the same as ProfilePacket.index. This gets only gets incremented when
  // encountering a ProfilePacket that is not continued.
  // This namespaces all other Source*Ids.
  using ProfileIndex = uint64_t;

  using SourceStringId = uint64_t;

  struct SourceMapping {
    SourceStringId build_id = 0;
    uint64_t offset = 0;
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
    uint64_t timestamp = 0;
    SourceCallstackId callstack_id = 0;
    uint64_t self_allocated = 0;
    uint64_t self_freed = 0;
    uint64_t alloc_count = 0;
    uint64_t free_count = 0;
  };

  explicit HeapProfileTracker(TraceProcessorContext* context);

  void AddString(ProfileIndex, SourceStringId, StringId);
  void AddMapping(ProfileIndex, SourceMappingId, const SourceMapping&);
  void AddFrame(ProfileIndex, SourceFrameId, const SourceFrame&);
  void AddCallstack(ProfileIndex, SourceCallstackId, const SourceCallstack&);

  void StoreAllocation(ProfileIndex, SourceAllocation);
  void ApplyAllAllocations();

  int64_t GetDatabaseFrameIdForTesting(ProfileIndex, SourceFrameId);

  ~HeapProfileTracker();

 private:
  void AddAllocation(ProfileIndex, const SourceAllocation&);

  base::Optional<StringId> FindString(ProfileIndex, SourceStringId);

  std::unordered_map<std::pair<ProfileIndex, SourceStringId>, StringId>
      string_map_;
  std::unordered_map<std::pair<ProfileIndex, SourceMappingId>, int64_t>
      mappings_;
  std::unordered_map<std::pair<ProfileIndex, SourceFrameId>, int64_t> frames_;
  std::unordered_map<std::pair<ProfileIndex, SourceCallstack>, int64_t>
      callstacks_from_frames_;
  std::unordered_map<std::pair<ProfileIndex, SourceCallstackId>, int64_t>
      callstacks_;

  std::vector<std::pair<ProfileIndex, SourceAllocation>> pending_allocs_;

  TraceProcessorContext* const context_;
  const StringId empty_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_HEAP_PROFILE_TRACKER_H_
