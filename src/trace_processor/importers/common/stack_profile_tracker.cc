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

#include "src/trace_processor/importers/common/stack_profile_tracker.h"

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/profiler_util.h"
#include "src/trace_processor/util/stack_traces_util.h"

namespace perfetto {
namespace trace_processor {

namespace {
std::string CleanBuildId(base::StringView build_id) {
  if (build_id.empty()) {
    return build_id.ToStdString();
  }
  // If the build_id is 33 characters long, we assume it's a Breakpad debug
  // identifier which is already in Hex and doesn't need conversion.
  // TODO(b/148109467): Remove workaround once all active Chrome versions
  // write raw bytes instead of a string as build_id.
  if (util::IsHexModuleId(build_id)) {
    return build_id.ToStdString();
  }

  return base::ToHex(build_id.data(), build_id.size());
}

}  // namespace

std::vector<FrameId> StackProfileTracker::JavaFramesForName(
    NameInPackage name) const {
  if (const auto* frames = java_frames_for_name_.Find(name); frames) {
    return *frames;
  }
  return {};
}

std::vector<MappingId> StackProfileTracker::FindMappingRow(
    StringId name,
    StringId build_id) const {
  if (const auto* mappings =
          mappings_by_name_and_build_id_.Find(std::make_pair(name, build_id));
      mappings) {
    return *mappings;
  }
  return {};
}

std::vector<FrameId> StackProfileTracker::FindFrameIds(MappingId mapping_id,
                                                       uint64_t rel_pc) const {
  if (const auto* frames =
          frame_by_mapping_and_rel_pc_.Find(std::make_pair(mapping_id, rel_pc));
      frames) {
    return *frames;
  }
  return {};
}

MappingId StackProfileTracker::InternMapping(
    const CreateMappingParams& params) {
  tables::StackProfileMappingTable::Row row;
  row.build_id = InternBuildId(params.build_id);
  row.exact_offset = static_cast<int64_t>(params.exact_offset);
  row.start_offset = static_cast<int64_t>(params.start_offset);
  row.start = static_cast<int64_t>(params.start);
  row.end = static_cast<int64_t>(params.end);
  row.load_bias = static_cast<int64_t>(params.load_bias);
  row.name = context_->storage->InternString(params.name);

  if (MappingId* id = mapping_unique_row_index_.Find(row); id) {
    return *id;
  }

  MappingId mapping_id =
      context_->storage->mutable_stack_profile_mapping_table()->Insert(row).id;
  mapping_unique_row_index_.Insert(row, mapping_id);
  mappings_by_name_and_build_id_[{row.name, row.build_id}].push_back(
      mapping_id);
  return mapping_id;
}

CallsiteId StackProfileTracker::InternCallsite(
    std::optional<CallsiteId> parent_callsite_id,
    FrameId frame_id,
    uint32_t depth) {
  tables::StackProfileCallsiteTable::Row row{depth, parent_callsite_id,
                                             frame_id};
  if (CallsiteId* id = callsite_unique_row_index_.Find(row); id) {
    return *id;
  }

  CallsiteId callsite_id =
      context_->storage->mutable_stack_profile_callsite_table()->Insert(row).id;
  callsite_unique_row_index_.Insert(row, callsite_id);
  return callsite_id;
}

FrameId StackProfileTracker::InternFrame(MappingId mapping_id,
                                         uint64_t rel_pc,
                                         base::StringView function_name) {
  tables::StackProfileFrameTable::Row row;
  row.mapping = mapping_id;
  row.rel_pc = static_cast<int64_t>(rel_pc);
  row.name = context_->storage->InternString(function_name);

  if (FrameId* id = frame_unique_row_index_.Find(row); id) {
    return *id;
  }

  FrameId frame_id =
      context_->storage->mutable_stack_profile_frame_table()->Insert(row).id;
  frame_unique_row_index_.Insert(row, frame_id);
  frame_by_mapping_and_rel_pc_[{mapping_id, rel_pc}].push_back(frame_id);

  if (function_name.find('.') != base::StringView::npos) {
    // Java frames always contain a '.'
    base::StringView mapping_name = context_->storage->GetString(
        context_->storage->stack_profile_mapping_table()
            .FindById(mapping_id)
            ->name());
    std::optional<std::string> package =
        PackageFromLocation(context_->storage.get(), mapping_name);
    if (package) {
      NameInPackage nip{row.name, context_->storage->InternString(
                                      base::StringView(*package))};
      java_frames_for_name_[nip].push_back(frame_id);
    } else if (mapping_name.find("/memfd:") == 0) {
      NameInPackage nip{row.name, context_->storage->InternString("memfd")};
      java_frames_for_name_[nip].push_back(frame_id);
    }
  }

  return frame_id;
}

StringId StackProfileTracker::InternBuildId(base::StringView build_id) {
  return context_->storage->InternString(
      base::StringView(CleanBuildId(build_id)));
}

}  // namespace trace_processor
}  // namespace perfetto
