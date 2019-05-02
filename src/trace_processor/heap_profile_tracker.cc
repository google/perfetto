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

#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

HeapProfileTracker::HeapProfileTracker(TraceProcessorContext* context)
    : context_(context), empty_(context_->storage->InternString({"", 0})) {}

HeapProfileTracker::~HeapProfileTracker() = default;

void HeapProfileTracker::AddString(ProfileIndex pidx,
                                   SourceStringId id,
                                   StringId str) {
  string_map_.emplace(std::make_pair(pidx, id), str);
}

void HeapProfileTracker::AddMapping(ProfileIndex pidx,
                                    SourceMappingId id,
                                    const SourceMapping& mapping) {
  auto opt_name_id = FindString(pidx, mapping.name_id);
  if (!opt_name_id)
    return;
  const StringId name_id = opt_name_id.value();

  auto opt_build_id = FindString(pidx, mapping.build_id);
  if (!opt_build_id)
    return;
  const StringId build_id = opt_build_id.value();

  int64_t cur_row =
      context_->storage->mutable_heap_profile_mappings()->FindOrInsert(
          build_id, static_cast<int64_t>(mapping.offset),
          static_cast<int64_t>(mapping.start),
          static_cast<int64_t>(mapping.end),
          static_cast<int64_t>(mapping.load_bias), name_id);
  mappings_.emplace(std::make_pair(pidx, id), cur_row);
}

void HeapProfileTracker::AddFrame(ProfileIndex pidx,
                                  SourceFrameId id,
                                  const SourceFrame& frame) {
  auto opt_str_id = FindString(pidx, frame.name_id);
  if (!opt_str_id)
    return;
  const StringId& str_id = opt_str_id.value();

  auto mapping_it = mappings_.find({pidx, frame.mapping_id});
  if (mapping_it == mappings_.end()) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_mapping_id);
    PERFETTO_DFATAL("Invalid mapping.");
    return;
  }
  int64_t mapping_row = mapping_it->second;

  int64_t cur_row =
      context_->storage->mutable_heap_profile_frames()->FindOrInsert(
          str_id, mapping_row, static_cast<int64_t>(frame.rel_pc));
  frames_.emplace(std::make_pair(pidx, id), cur_row);
}

void HeapProfileTracker::AddCallstack(ProfileIndex pidx,
                                      SourceCallstackId id,
                                      const SourceCallstack& frame_ids) {
  int64_t parent_id = 0;
  for (size_t depth = 0; depth < frame_ids.size(); ++depth) {
    std::vector<uint64_t> frame_subset = frame_ids;
    frame_subset.resize(depth + 1);
    auto self_it = callstacks_from_frames_.find({pidx, frame_subset});
    if (self_it != callstacks_from_frames_.end()) {
      parent_id = self_it->second;
      continue;
    }

    uint64_t frame_id = frame_ids[depth];
    auto it = frames_.find({pidx, frame_id});
    if (it == frames_.end()) {
      context_->storage->IncrementStats(stats::heapprofd_invalid_frame_id);
      PERFETTO_DFATAL("Unknown frames.");
      return;
    }
    int64_t frame_row = it->second;
    int64_t self_id =
        context_->storage->mutable_heap_profile_callsites()->FindOrInsert(
            static_cast<int64_t>(depth), parent_id, frame_row);
    parent_id = self_id;
  }
  callstacks_.emplace(std::make_pair(pidx, id), parent_id);
}

void HeapProfileTracker::AddAllocation(ProfileIndex pidx,
                                       const SourceAllocation& alloc) {
  auto it = callstacks_.find({pidx, alloc.callstack_id});
  if (it == callstacks_.end()) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_callstack_id);
    PERFETTO_DFATAL("Unknown callstack %" PRIu64 " : %zu", alloc.callstack_id,
                    callstacks_.size());
    return;
  }
  context_->storage->mutable_heap_profile_allocations()->Insert(
      static_cast<int64_t>(alloc.timestamp), static_cast<int64_t>(alloc.pid),
      static_cast<int64_t>(it->second), static_cast<int64_t>(alloc.alloc_count),
      static_cast<int64_t>(alloc.self_allocated));
  context_->storage->mutable_heap_profile_allocations()->Insert(
      static_cast<int64_t>(alloc.timestamp), static_cast<int64_t>(alloc.pid),
      static_cast<int64_t>(it->second), -static_cast<int64_t>(alloc.free_count),
      -static_cast<int64_t>(alloc.self_freed));
}

void HeapProfileTracker::StoreAllocation(ProfileIndex pidx,
                                         SourceAllocation alloc) {
  pending_allocs_.emplace_back(pidx, std::move(alloc));
}

void HeapProfileTracker::ApplyAllAllocations() {
  for (const auto& p : pending_allocs_)
    AddAllocation(p.first, p.second);
}

int64_t HeapProfileTracker::GetDatabaseFrameIdForTesting(
    ProfileIndex pidx,
    SourceFrameId frame_id) {
  auto it = frames_.find({pidx, frame_id});
  if (it == frames_.end()) {
    PERFETTO_DFATAL("Invalid frame.");
    return -1;
  }
  return it->second;
}

base::Optional<StringId> HeapProfileTracker::FindString(ProfileIndex pidx,
                                                        SourceStringId id) {
  base::Optional<StringId> res;
  if (id == 0) {
    res = empty_;
    return res;
  }

  auto it = string_map_.find({pidx, id});
  if (it == string_map_.end()) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return res;
  }
  res = it->second;
  return res;
}

}  // namespace trace_processor
}  // namespace perfetto
