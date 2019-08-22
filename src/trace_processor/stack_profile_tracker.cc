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

#include "src/trace_processor/stack_profile_tracker.h"

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

StackProfileTracker::InternLookup::~InternLookup() = default;

StackProfileTracker::StackProfileTracker(TraceProcessorContext* context)
    : context_(context), empty_(context_->storage->InternString({"", 0})) {}

StackProfileTracker::~StackProfileTracker() = default;

void StackProfileTracker::AddString(SourceStringId id, StringId str) {
  string_map_.emplace(id, str);
}

int64_t StackProfileTracker::AddMapping(SourceMappingId id,
                                        const SourceMapping& mapping,
                                        const InternLookup* intern_lookup) {
  auto opt_name_id = FindString(mapping.name_id, intern_lookup);
  if (!opt_name_id) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return -1;
  }
  const StringId name_id = opt_name_id.value();

  auto opt_build_id = FindString(mapping.build_id, intern_lookup);
  if (!opt_build_id) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return -1;
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

  TraceStorage::StackProfileMappings::Row row{
      build_id,
      static_cast<int64_t>(mapping.exact_offset),
      static_cast<int64_t>(mapping.start_offset),
      static_cast<int64_t>(mapping.start),
      static_cast<int64_t>(mapping.end),
      static_cast<int64_t>(mapping.load_bias),
      name_id};

  int64_t cur_row;
  auto it = mapping_idx_.find(row);
  if (it != mapping_idx_.end()) {
    cur_row = it->second;
  } else {
    cur_row = context_->storage->mutable_stack_profile_mappings()->Insert(row);
    mapping_idx_.emplace(row, cur_row);
  }
  mappings_.emplace(id, cur_row);
  return cur_row;
}

int64_t StackProfileTracker::AddFrame(SourceFrameId id,
                                      const SourceFrame& frame,
                                      const InternLookup* intern_lookup) {
  auto opt_str_id = FindString(frame.name_id, intern_lookup);
  if (!opt_str_id) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return -1;
  }
  const StringId& str_id = opt_str_id.value();

  auto maybe_mapping = FindMapping(frame.mapping_id, intern_lookup);
  if (!maybe_mapping) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_mapping_id);
    PERFETTO_DFATAL("Invalid mapping.");
    return -1;
  }
  int64_t mapping_row = *maybe_mapping;

  TraceStorage::StackProfileFrames::Row row{str_id, mapping_row,
                                            static_cast<int64_t>(frame.rel_pc)};

  int64_t cur_row;
  auto it = frame_idx_.find(row);
  if (it != frame_idx_.end()) {
    cur_row = it->second;
  } else {
    cur_row = context_->storage->mutable_stack_profile_frames()->Insert(row);
    frame_idx_.emplace(row, cur_row);
  }
  frames_.emplace(id, cur_row);
  return cur_row;
}

int64_t StackProfileTracker::AddCallstack(SourceCallstackId id,
                                          const SourceCallstack& frame_ids,
                                          const InternLookup* intern_lookup) {
  // TODO(fmayer): This should be NULL.
  int64_t parent_id = -1;
  for (size_t depth = 0; depth < frame_ids.size(); ++depth) {
    std::vector<SourceFrameId> frame_subset = frame_ids;
    frame_subset.resize(depth + 1);
    auto self_it = callstacks_from_frames_.find(frame_subset);
    if (self_it != callstacks_from_frames_.end()) {
      parent_id = self_it->second;
      continue;
    }

    SourceFrameId frame_id = frame_ids[depth];
    auto maybe_frame_row = FindFrame(frame_id, intern_lookup);
    if (!maybe_frame_row) {
      context_->storage->IncrementStats(stats::stackprofile_invalid_frame_id);
      PERFETTO_DFATAL("Unknown frames.");
      return -1;
    }
    int64_t frame_row = *maybe_frame_row;

    TraceStorage::StackProfileCallsites::Row row{static_cast<int64_t>(depth),
                                                 parent_id, frame_row};

    int64_t self_id;
    auto callsite_it = callsite_idx_.find(row);
    if (callsite_it != callsite_idx_.end()) {
      self_id = callsite_it->second;
    } else {
      self_id =
          context_->storage->mutable_stack_profile_callsites()->Insert(row);
      callsite_idx_.emplace(row, self_id);
    }
    parent_id = self_id;
  }
  callstacks_.emplace(id, parent_id);
  return parent_id;
}

int64_t StackProfileTracker::GetDatabaseFrameIdForTesting(
    SourceFrameId frame_id) {
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    PERFETTO_DFATAL("Invalid frame.");
    return -1;
  }
  return it->second;
}

base::Optional<StringId> StackProfileTracker::FindString(
    SourceStringId id,
    const InternLookup* intern_lookup) {
  base::Optional<StringId> res;
  if (id == 0) {
    res = empty_;
    return res;
  }

  auto it = string_map_.find(id);
  if (it == string_map_.end()) {
    if (intern_lookup) {
      auto interned_str = intern_lookup->GetString(id);
      if (interned_str)
        return interned_str;
    }
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return res;
  }
  res = it->second;
  return res;
}

base::Optional<int64_t> StackProfileTracker::FindMapping(
    SourceMappingId mapping_id,
    const InternLookup* intern_lookup) {
  base::Optional<int64_t> res;
  auto it = mappings_.find(mapping_id);
  if (it == mappings_.end()) {
    if (intern_lookup) {
      auto interned_mapping = intern_lookup->GetMapping(mapping_id);
      if (interned_mapping) {
        res = AddMapping(mapping_id, *interned_mapping, intern_lookup);
        return res;
      }
    }
    context_->storage->IncrementStats(stats::stackprofile_invalid_mapping_id);
    PERFETTO_DFATAL("Unknown mapping %" PRIu64 " : %zu", mapping_id,
                    mappings_.size());
    return res;
  }
  res = it->second;
  return res;
}

base::Optional<int64_t> StackProfileTracker::FindFrame(
    SourceFrameId frame_id,
    const InternLookup* intern_lookup) {
  base::Optional<int64_t> res;
  auto it = frames_.find(frame_id);
  if (it == frames_.end()) {
    if (intern_lookup) {
      auto interned_frame = intern_lookup->GetFrame(frame_id);
      if (interned_frame) {
        res = AddFrame(frame_id, *interned_frame, intern_lookup);
        return res;
      }
    }
    context_->storage->IncrementStats(stats::stackprofile_invalid_frame_id);
    PERFETTO_DFATAL("Unknown frame %" PRIu64 " : %zu", frame_id,
                    frames_.size());
    return res;
  }
  res = it->second;
  return res;
}

base::Optional<int64_t> StackProfileTracker::FindCallstack(
    SourceCallstackId callstack_id,
    const InternLookup* intern_lookup) {
  base::Optional<int64_t> res;
  auto it = callstacks_.find(callstack_id);
  if (it == callstacks_.end()) {
    auto interned_callstack = intern_lookup->GetCallstack(callstack_id);
    if (interned_callstack) {
      res = AddCallstack(callstack_id, *interned_callstack, intern_lookup);
      return res;
    }
    context_->storage->IncrementStats(stats::stackprofile_invalid_callstack_id);
    PERFETTO_DFATAL("Unknown callstack %" PRIu64 " : %zu", callstack_id,
                    callstacks_.size());
    return res;
  }
  res = it->second;
  return res;
}

void StackProfileTracker::ClearIndices() {
  string_map_.clear();
  mappings_.clear();
  callstacks_from_frames_.clear();
  callstacks_.clear();
  // We intentionally hold on to the frames_ mappings - we will use them
  // if we encounter any ProfiledFrameSymbols packets for symbolizing.
}

void StackProfileTracker::SetFrameName(SourceFrameId source_frame_id,
                                       SourceStringId function_name_id,
                                       const InternLookup* intern_lookup) {
  auto maybe_frame_row = FindFrame(source_frame_id, intern_lookup);
  if (!maybe_frame_row) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_frame_id);
    PERFETTO_DFATAL_OR_ELOG("Unknown frame iid %" PRIu64 " in symbols.",
                            source_frame_id);
  }
  auto maybe_name_id = FindString(function_name_id, intern_lookup);
  if (!maybe_name_id) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    PERFETTO_DFATAL_OR_ELOG("Invalid string iid %" PRIu64 " in symbols.",
                            function_name_id);
  }

  size_t frame_row = static_cast<size_t>(*maybe_frame_row);
  context_->storage->mutable_stack_profile_frames()->SetFrameName(
      frame_row, *maybe_name_id);
}

}  // namespace trace_processor
}  // namespace perfetto
