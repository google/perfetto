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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STACK_PROFILE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STACK_PROFILE_TRACKER_H_

#include <deque>
#include <unordered_map>

#include "perfetto/ext/base/optional.h"

#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables.h"

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
struct hash<std::pair<uint32_t, perfetto::trace_processor::CallsiteId>> {
  using argument_type =
      std::pair<uint32_t, perfetto::trace_processor::CallsiteId>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    return std::hash<uint32_t>{}(p.first) ^
           std::hash<uint32_t>{}(p.second.value);
  }
};

template <>
struct hash<std::pair<uint32_t, perfetto::trace_processor::MappingId>> {
  using argument_type =
      std::pair<uint32_t, perfetto::trace_processor::MappingId>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    return std::hash<uint32_t>{}(p.first) ^
           std::hash<uint32_t>{}(p.second.value);
  }
};

template <>
struct hash<std::pair<uint32_t, perfetto::trace_processor::FrameId>> {
  using argument_type = std::pair<uint32_t, perfetto::trace_processor::FrameId>;
  using result_type = size_t;

  result_type operator()(const argument_type& p) const {
    return std::hash<uint32_t>{}(p.first) ^
           std::hash<uint32_t>{}(p.second.value);
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

struct NameInPackage {
  StringId name;
  StringId package;

  bool operator<(const NameInPackage& b) const {
    return std::tie(name, package) < std::tie(b.name, b.package);
  }
};

class TraceProcessorContext;

class GlobalStackProfileTracker {
 public:
  std::vector<MappingId> FindMappingRow(StringId name,
                                        StringId build_id) const {
    auto it = stack_profile_mapping_index_.find(std::make_pair(name, build_id));
    if (it == stack_profile_mapping_index_.end())
      return {};
    return it->second;
  }

  void InsertMappingId(StringId name, StringId build_id, MappingId row) {
    auto pair = std::make_pair(name, build_id);
    stack_profile_mapping_index_[pair].emplace_back(row);
  }

  std::vector<FrameId> FindFrameIds(MappingId mapping_row,
                                    uint64_t rel_pc) const {
    auto it =
        stack_profile_frame_index_.find(std::make_pair(mapping_row, rel_pc));
    if (it == stack_profile_frame_index_.end())
      return {};
    return it->second;
  }

  void InsertFrameRow(MappingId mapping_row, uint64_t rel_pc, FrameId row) {
    auto pair = std::make_pair(mapping_row, rel_pc);
    stack_profile_frame_index_[pair].emplace_back(row);
  }

  const std::vector<tables::StackProfileFrameTable::Id>* JavaFramesForName(
      NameInPackage name) {
    auto it = java_frames_for_name_.find(name);
    if (it == java_frames_for_name_.end())
      return nullptr;
    return &it->second;
  }

  void InsertJavaFrameForName(NameInPackage name,
                              tables::StackProfileFrameTable::Id id) {
    java_frames_for_name_[name].push_back(id);
  }

 private:
  using MappingKey = std::pair<StringId /* name */, StringId /* build id */>;
  std::map<MappingKey, std::vector<MappingId>> stack_profile_mapping_index_;

  using FrameKey = std::pair<MappingId, uint64_t /* rel_pc */>;
  std::map<FrameKey, std::vector<FrameId>> stack_profile_frame_index_;

  std::map<NameInPackage, std::vector<tables::StackProfileFrameTable::Id>>
      java_frames_for_name_;
};

// TODO(lalitm): Overhaul this class to make row vs id consistent and use
// base::Optional instead of int64_t.
class SequenceStackProfileTracker {
 public:
  using SourceStringId = uint64_t;

  enum class InternedStringType {
    kMappingPath,
    kBuildId,
    kFunctionName,
  };

  struct SourceMapping {
    SourceStringId build_id = 0;
    uint64_t exact_offset = 0;
    uint64_t start_offset = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t load_bias = 0;
    std::vector<SourceStringId> name_ids;
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

    virtual base::Optional<base::StringView> GetString(
        SourceStringId,
        InternedStringType) const = 0;
    virtual base::Optional<SourceMapping> GetMapping(SourceMappingId) const = 0;
    virtual base::Optional<SourceFrame> GetFrame(SourceFrameId) const = 0;
    virtual base::Optional<SourceCallstack> GetCallstack(
        SourceCallstackId) const = 0;
  };

  explicit SequenceStackProfileTracker(TraceProcessorContext* context);
  ~SequenceStackProfileTracker();

  void AddString(SourceStringId, base::StringView);
  base::Optional<MappingId> AddMapping(
      SourceMappingId,
      const SourceMapping&,
      const InternLookup* intern_lookup = nullptr);
  base::Optional<FrameId> AddFrame(SourceFrameId,
                                   const SourceFrame&,
                                   const InternLookup* intern_lookup = nullptr);
  base::Optional<CallsiteId> AddCallstack(
      SourceCallstackId,
      const SourceCallstack&,
      const InternLookup* intern_lookup = nullptr);

  FrameId GetDatabaseFrameIdForTesting(SourceFrameId);

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
  base::Optional<StringId> FindAndInternString(
      SourceStringId,
      const InternLookup* intern_lookup,
      InternedStringType type);
  base::Optional<std::string> FindOrInsertString(
      SourceStringId,
      const InternLookup* intern_lookup,
      InternedStringType type);
  base::Optional<MappingId> FindOrInsertMapping(
      SourceMappingId,
      const InternLookup* intern_lookup);
  base::Optional<FrameId> FindOrInsertFrame(SourceFrameId,
                                            const InternLookup* intern_lookup);

  base::Optional<CallsiteId> FindOrInsertCallstack(
      SourceCallstackId,
      const InternLookup* intern_lookup);

  // Clear indices when they're no longer needed.
  void ClearIndices();

 private:
  StringId GetEmptyStringId();

  std::unordered_map<SourceStringId, std::string> string_map_;

  // Mapping from ID of mapping / frame / callstack in original trace and the
  // index in the respective table it was inserted into.
  std::unordered_map<SourceMappingId, MappingId> mapping_ids_;
  std::unordered_map<SourceFrameId, FrameId> frame_ids_;
  std::unordered_map<SourceCallstackId, CallsiteId> callstack_ids_;

  // TODO(oysteine): Share these indices between the StackProfileTrackers,
  // since they're not sequence-specific.
  //
  // Mapping from content of database row to the index of the raw.
  std::unordered_map<tables::StackProfileMappingTable::Row, MappingId>
      mapping_idx_;
  std::unordered_map<tables::StackProfileFrameTable::Row, FrameId> frame_idx_;
  std::unordered_map<tables::StackProfileCallsiteTable::Row, CallsiteId>
      callsite_idx_;

  TraceProcessorContext* const context_;
  StringId empty_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STACK_PROFILE_TRACKER_H_
