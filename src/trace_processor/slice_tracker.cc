/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <stdint.h>

#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

SliceTracker::SliceTracker(TraceProcessorContext* context)
    : context_(context) {}

SliceTracker::~SliceTracker() = default;

void SliceTracker::Begin(uint64_t timestamp,
                         UniqueTid utid,
                         StringId cat,
                         StringId name) {
  auto& stack = threads_[utid];
  MaybeCloseStack(timestamp, stack);
  stack.emplace_back(Slice{cat, name, timestamp, 0});
}

void SliceTracker::Scoped(uint64_t timestamp,
                          UniqueTid utid,
                          StringId cat,
                          StringId name,
                          uint64_t duration) {
  auto& stack = threads_[utid];
  MaybeCloseStack(timestamp, stack);
  stack.emplace_back(Slice{cat, name, timestamp, timestamp + duration});
  CompleteSlice(utid);
}

void SliceTracker::End(uint64_t timestamp,
                       UniqueTid utid,
                       StringId cat,
                       StringId name) {
  auto& stack = threads_[utid];
  MaybeCloseStack(timestamp, stack);
  PERFETTO_CHECK(!stack.empty());

  PERFETTO_CHECK(cat == 0 || stack.back().cat_id == cat);
  PERFETTO_CHECK(name == 0 || stack.back().name_id == name);

  Slice& slice = stack.back();
  slice.end_ts = timestamp;

  CompleteSlice(utid);
  // TODO(primiano): auto-close B slices left open at the end.
}

void SliceTracker::CompleteSlice(UniqueTid utid) {
  auto& stack = threads_[utid];
  if (stack.size() >= std::numeric_limits<uint8_t>::max()) {
    stack.pop_back();
    return;
  }
  const uint8_t depth = static_cast<uint8_t>(stack.size()) - 1;

  uint64_t parent_stack_id, stack_id;
  std::tie(parent_stack_id, stack_id) = GetStackHashes(stack);

  Slice& slice = stack.back();
  auto* slices = context_->storage->mutable_nestable_slices();
  slices->AddSlice(slice.start_ts, slice.end_ts - slice.start_ts, utid, 0,
                   slice.name_id, depth, stack_id, parent_stack_id);

  stack.pop_back();
}

void SliceTracker::MaybeCloseStack(uint64_t ts, SlicesStack& stack) {
  bool check_only = false;
  for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
    const Slice& slice = stack[size_t(i)];
    if (slice.end_ts == 0) {
      check_only = true;
    }

    if (check_only) {
      PERFETTO_DCHECK(ts >= slice.start_ts);
      PERFETTO_DCHECK(slice.end_ts == 0 || ts <= slice.end_ts);
      continue;
    }

    if (slice.end_ts <= ts) {
      stack.pop_back();
    }
  }
}

// Returns <parent_stack_id, stack_id>, where
// |parent_stack_id| == hash(stack_id - last slice).
std::tuple<uint64_t, uint64_t> SliceTracker::GetStackHashes(
    const SlicesStack& stack) {
  PERFETTO_DCHECK(!stack.empty());
  std::string s;
  s.reserve(stack.size() * sizeof(uint64_t) * 2);
  constexpr uint64_t kMask = uint64_t(-1) >> 1;
  uint64_t parent_stack_id = 0;
  for (size_t i = 0; i < stack.size(); i++) {
    if (i == stack.size() - 1)
      parent_stack_id = i > 0 ? (std::hash<std::string>{}(s)) & kMask : 0;
    const Slice& slice = stack[i];
    s.append(reinterpret_cast<const char*>(&slice.cat_id),
             sizeof(slice.cat_id));
    s.append(reinterpret_cast<const char*>(&slice.name_id),
             sizeof(slice.name_id));
  }
  uint64_t stack_id = (std::hash<std::string>{}(s)) & kMask;
  return std::make_tuple(parent_stack_id, stack_id);
}

}  // namespace trace_processor
}  // namespace perfetto
