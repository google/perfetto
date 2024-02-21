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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_STACK_PROFILE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_STACK_PROFILE_TRACKER_H_

#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"

namespace perfetto {
namespace trace_processor {

struct NameInPackage {
  StringId name;
  StringId package;

  bool operator==(const NameInPackage& b) const {
    return std::tie(name, package) == std::tie(b.name, b.package);
  }

  struct Hasher {
    size_t operator()(const NameInPackage& o) const {
      return static_cast<size_t>(
          base::Hasher::Combine(o.name.raw_id(), o.package.raw_id()));
    }
  };
};

class TraceProcessorContext;

class StackProfileTracker {
 public:
  struct CreateMappingParams {
    base::StringView build_id;
    uint64_t exact_offset;
    uint64_t start_offset;
    uint64_t start;
    uint64_t end;
    uint64_t load_bias;
    base::StringView name;
  };

  explicit StackProfileTracker(TraceProcessorContext* context)
      : context_(context) {}

  std::vector<FrameId> JavaFramesForName(NameInPackage name) const;
  std::vector<MappingId> FindMappingRow(StringId name, StringId build_id) const;
  std::vector<FrameId> FindFrameIds(MappingId mapping_id,
                                    uint64_t rel_pc) const;

  MappingId InternMapping(const CreateMappingParams& params);
  CallsiteId InternCallsite(std::optional<CallsiteId> parent_callsite_id,
                            FrameId frame_id,
                            uint32_t depth);
  FrameId InternFrame(MappingId mapping_id,
                      uint64_t rel_pc,
                      base::StringView function_name);

 private:
  StringId InternBuildId(base::StringView build_id);

  TraceProcessorContext* const context_;
  base::FlatHashMap<tables::StackProfileMappingTable::Row, MappingId>
      mapping_unique_row_index_;
  base::FlatHashMap<tables::StackProfileCallsiteTable::Row, CallsiteId>
      callsite_unique_row_index_;
  base::FlatHashMap<tables::StackProfileFrameTable::Row, FrameId>
      frame_unique_row_index_;

  struct MappingHasher {
    size_t operator()(const std::pair<StringId, StringId>& o) const {
      return static_cast<size_t>(
          base::Hasher::Combine(o.first.raw_id(), o.second.raw_id()));
    }
  };
  base::FlatHashMap<std::pair<StringId, StringId>,
                    std::vector<MappingId>,
                    MappingHasher>
      mappings_by_name_and_build_id_;

  struct FrameHasher {
    size_t operator()(const std::pair<MappingId, uint64_t>& o) const {
      return static_cast<size_t>(
          base::Hasher::Combine(o.first.value, o.second));
    }
  };
  base::FlatHashMap<std::pair<MappingId, uint64_t>,
                    std::vector<FrameId>,
                    FrameHasher>
      frame_by_mapping_and_rel_pc_;

  base::FlatHashMap<NameInPackage, std::vector<FrameId>, NameInPackage::Hasher>
      java_frames_for_name_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_STACK_PROFILE_TRACKER_H_
