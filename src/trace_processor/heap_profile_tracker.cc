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
namespace {

std::string ToHex(const char* build_id, size_t size) {
  std::string hex_build_id(2 * size + 1, 'x');
  for (size_t i = 0; i < size; ++i) {
    // snprintf prints 3 characters, the two hex digits and a null byte. As we
    // write left to write, we keep overwriting the nullbytes, except for the
    // last call to snprintf.
    snprintf(&(hex_build_id[2 * i]), 3, "%02hhx", build_id[i]);
  }
  // Remove the trailing nullbyte produced by the last snprintf.
  hex_build_id.resize(2 * size);
  return hex_build_id;
}

}  // namespace

HeapProfileTracker::HeapProfileTracker(TraceProcessorContext* context)
    : context_(context), empty_(context_->storage->InternString({"", 0})) {}

HeapProfileTracker::~HeapProfileTracker() = default;

void HeapProfileTracker::AddString(SourceStringId id, StringId str) {
  string_map_.emplace(id, str);
}

void HeapProfileTracker::AddMapping(SourceMappingId id,
                                    const SourceMapping& mapping) {
  auto opt_name_id = FindString(mapping.name_id);
  if (!opt_name_id) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return;
  }
  const StringId name_id = opt_name_id.value();

  auto opt_build_id = FindString(mapping.build_id);
  if (!opt_build_id) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return;
  }
  const StringId raw_build_id = opt_build_id.value();
  NullTermStringView raw_build_id_str =
      context_->storage->GetString(raw_build_id);
  StringId build_id = empty_;
  if (raw_build_id_str.size() > 0) {
    std::string hex_build_id =
        ToHex(raw_build_id_str.c_str(), raw_build_id_str.size());
    build_id = context_->storage->InternString(base::StringView(hex_build_id));
  }

  TraceStorage::HeapProfileMappings::Row row{
      build_id,
      static_cast<int64_t>(mapping.offset),
      static_cast<int64_t>(mapping.start),
      static_cast<int64_t>(mapping.end),
      static_cast<int64_t>(mapping.load_bias),
      name_id};

  int64_t cur_row;
  auto it = mapping_idx_.find(row);
  if (it != mapping_idx_.end()) {
    cur_row = it->second;
  } else {
    cur_row = context_->storage->mutable_heap_profile_mappings()->Insert(row);
    mapping_idx_.emplace(row, cur_row);
  }
  mappings_.emplace(id, cur_row);
}

void HeapProfileTracker::AddFrame(SourceFrameId id, const SourceFrame& frame) {
  auto opt_str_id = FindString(frame.name_id);
  if (!opt_str_id) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return;
  }
  const StringId& str_id = opt_str_id.value();

  auto mapping_it = mappings_.find(frame.mapping_id);
  if (mapping_it == mappings_.end()) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_mapping_id);
    PERFETTO_DFATAL("Invalid mapping.");
    return;
  }
  int64_t mapping_row = mapping_it->second;

  TraceStorage::HeapProfileFrames::Row row{str_id, mapping_row,
                                           static_cast<int64_t>(frame.rel_pc)};

  int64_t cur_row;
  auto it = frame_idx_.find(row);
  if (it != frame_idx_.end()) {
    cur_row = it->second;
  } else {
    cur_row = context_->storage->mutable_heap_profile_frames()->Insert(row);
    frame_idx_.emplace(row, cur_row);
  }
  frames_.emplace(id, cur_row);
}

void HeapProfileTracker::AddCallstack(SourceCallstackId id,
                                      const SourceCallstack& frame_ids) {
  // TODO(fmayer): This should be NULL.
  int64_t parent_id = -1;
  for (size_t depth = 0; depth < frame_ids.size(); ++depth) {
    std::vector<uint64_t> frame_subset = frame_ids;
    frame_subset.resize(depth + 1);
    auto self_it = callstacks_from_frames_.find(frame_subset);
    if (self_it != callstacks_from_frames_.end()) {
      parent_id = self_it->second;
      continue;
    }

    uint64_t frame_id = frame_ids[depth];
    auto it = frames_.find(frame_id);
    if (it == frames_.end()) {
      context_->storage->IncrementStats(stats::heapprofd_invalid_frame_id);
      PERFETTO_DFATAL("Unknown frames.");
      return;
    }
    int64_t frame_row = it->second;

    TraceStorage::HeapProfileCallsites::Row row{static_cast<int64_t>(depth),
                                                parent_id, frame_row};

    int64_t self_id;
    auto callsite_it = callsite_idx_.find(row);
    if (callsite_it != callsite_idx_.end()) {
      self_id = callsite_it->second;
    } else {
      self_id =
          context_->storage->mutable_heap_profile_callsites()->Insert(row);
      callsite_idx_.emplace(row, self_id);
    }
    parent_id = self_id;
  }
  callstacks_.emplace(id, parent_id);
}

void HeapProfileTracker::AddAllocation(const SourceAllocation& alloc) {
  auto it = callstacks_.find(alloc.callstack_id);
  if (it == callstacks_.end()) {
    context_->storage->IncrementStats(stats::heapprofd_invalid_callstack_id);
    PERFETTO_DFATAL("Unknown callstack %" PRIu64 " : %zu", alloc.callstack_id,
                    callstacks_.size());
    return;
  }

  int64_t callstack_id = static_cast<int64_t>(it->second);

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

void HeapProfileTracker::CommitAllocations() {
  for (const auto& p : pending_allocs_)
    AddAllocation(p);
  pending_allocs_.clear();
}

void HeapProfileTracker::FinalizeProfile() {
  CommitAllocations();

  string_map_.clear();
  mappings_.clear();
  frames_.clear();
  callstacks_from_frames_.clear();
  callstacks_.clear();
}

int64_t HeapProfileTracker::GetDatabaseFrameIdForTesting(
    SourceFrameId frame_id) {
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    PERFETTO_DFATAL("Invalid frame.");
    return -1;
  }
  return it->second;
}

base::Optional<StringId> HeapProfileTracker::FindString(SourceStringId id) {
  base::Optional<StringId> res;
  if (id == 0) {
    res = empty_;
    return res;
  }

  auto it = string_map_.find(id);
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
