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

#include <limits>

#include <stdint.h>

#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

SliceTracker::SliceTracker(TraceProcessorContext* context)
    : context_(context) {}

SliceTracker::~SliceTracker() = default;

void SliceTracker::BeginAndroid(uint64_t timestamp,
                                uint32_t ftrace_tid,
                                uint32_t atrace_tid,
                                StringId cat,
                                StringId name) {
  UniqueTid utid =
      context_->process_tracker->UpdateThread(timestamp, atrace_tid, 0);
  ftrace_to_atrace_pid_[ftrace_tid] = atrace_tid;
  Begin(timestamp, utid, cat, name);
}

void SliceTracker::Begin(uint64_t timestamp,
                         UniqueTid utid,
                         StringId cat,
                         StringId name) {
  MaybeCloseStack(timestamp, &threads_[utid]);
  StartSlice(timestamp, 0, utid, cat, name);
}

void SliceTracker::Scoped(uint64_t timestamp,
                          UniqueTid utid,
                          StringId cat,
                          StringId name,
                          uint64_t duration) {
  MaybeCloseStack(timestamp, &threads_[utid]);
  StartSlice(timestamp, duration, utid, cat, name);
  CompleteSlice(utid);
}

void SliceTracker::StartSlice(uint64_t timestamp,
                              uint64_t duration,
                              UniqueTid utid,
                              StringId cat,
                              StringId name) {
  auto* stack = &threads_[utid];
  auto* slices = context_->storage->mutable_nestable_slices();

  const uint8_t depth = static_cast<uint8_t>(stack->size());
  if (depth >= std::numeric_limits<uint8_t>::max()) {
    PERFETTO_DFATAL("Slices with too large depth found.");
    return;
  }
  uint64_t parent_stack_id = depth == 0 ? 0 : slices->stack_ids().back();
  size_t slice_idx = slices->AddSlice(timestamp, duration, utid, cat, name,
                                      depth, 0, parent_stack_id);
  stack->emplace_back(slice_idx);

  slices->set_stack_id(slice_idx, GetStackHash(*stack));
}

void SliceTracker::EndAndroid(uint64_t timestamp,
                              uint32_t ftrace_tid,
                              uint32_t atrace_tid) {
  uint32_t actual_tid = ftrace_to_atrace_pid_[ftrace_tid];
  if (atrace_tid != 0 && atrace_tid != actual_tid) {
    PERFETTO_DLOG("Mismatched atrace pid %u and looked up pid %u", atrace_tid,
                  actual_tid);
  }
  if (actual_tid == 0)
    return;
  UniqueTid utid =
      context_->process_tracker->UpdateThread(timestamp, actual_tid, 0);
  End(timestamp, utid);
}

void SliceTracker::End(uint64_t timestamp,
                       UniqueTid utid,
                       StringId cat,
                       StringId name) {
  MaybeCloseStack(timestamp, &threads_[utid]);

  const auto& stack = threads_[utid];
  if (stack.empty())
    return;

  auto* slices = context_->storage->mutable_nestable_slices();
  size_t slice_idx = stack.back();

  PERFETTO_CHECK(cat == 0 || slices->cats()[slice_idx] == cat);
  PERFETTO_CHECK(name == 0 || slices->names()[slice_idx] == name);

  slices->set_duration(slice_idx, timestamp - slices->start_ns()[slice_idx]);

  CompleteSlice(utid);
  // TODO(primiano): auto-close B slices left open at the end.
}

void SliceTracker::CompleteSlice(UniqueTid utid) {
  threads_[utid].pop_back();
}

void SliceTracker::MaybeCloseStack(uint64_t ts, SlicesStack* stack) {
  const auto& slices = context_->storage->nestable_slices();
  bool check_only = false;
  for (int i = static_cast<int>(stack->size()) - 1; i >= 0; i--) {
    size_t slice_idx = (*stack)[static_cast<size_t>(i)];

    uint64_t start_ts = slices.start_ns()[slice_idx];
    uint64_t dur = slices.durations()[slice_idx];
    uint64_t end_ts = start_ts + dur;
    if (dur == 0) {
      check_only = true;
    }

    if (check_only) {
      PERFETTO_DCHECK(ts >= start_ts);
      PERFETTO_DCHECK(dur == 0 || ts <= end_ts);
      continue;
    }

    if (end_ts <= ts) {
      stack->pop_back();
    }
  }
}

uint64_t SliceTracker::GetStackHash(const SlicesStack& stack) {
  PERFETTO_DCHECK(!stack.empty());

  const auto& slices = context_->storage->nestable_slices();

  std::string s;
  s.reserve(stack.size() * sizeof(uint64_t) * 2);
  for (size_t i = 0; i < stack.size(); i++) {
    size_t slice_idx = stack[i];
    s.append(reinterpret_cast<const char*>(&slices.cats()[slice_idx]),
             sizeof(slices.cats()[slice_idx]));
    s.append(reinterpret_cast<const char*>(&slices.names()[slice_idx]),
             sizeof(slices.names()[slice_idx]));
  }
  constexpr uint64_t kMask = uint64_t(-1) >> 1;
  return (std::hash<std::string>{}(s)) & kMask;
}

}  // namespace trace_processor
}  // namespace perfetto
