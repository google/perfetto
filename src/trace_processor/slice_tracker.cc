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

#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {
namespace {
// Slices which have been opened but haven't been closed yet will be marked
// with this duration placeholder.
constexpr int64_t kPendingDuration = -1;
}  // namespace

SliceTracker::SliceTracker(TraceProcessorContext* context)
    : context_(context) {}

SliceTracker::~SliceTracker() = default;

base::Optional<uint32_t> SliceTracker::BeginAndroid(int64_t timestamp,
                                                    uint32_t ftrace_tid,
                                                    uint32_t atrace_tgid,
                                                    StringId category,
                                                    StringId name) {
  UniqueTid utid =
      context_->process_tracker->UpdateThread(ftrace_tid, atrace_tgid);
  ftrace_to_atrace_tgid_[ftrace_tid] = atrace_tgid;
  return Begin(timestamp, utid, RefType::kRefUtid, category, name);
}

base::Optional<uint32_t> SliceTracker::Begin(int64_t timestamp,
                                             int64_t ref,
                                             RefType ref_type,
                                             StringId category,
                                             StringId name,
                                             SetArgsCallback args_callback) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    context_->storage->IncrementStats(stats::slice_out_of_order);
    return base::nullopt;
  }
  prev_timestamp_ = timestamp;

  MaybeCloseStack(timestamp, &stacks_[{ref, ref_type}]);
  return StartSlice(timestamp, kPendingDuration, ref, ref_type, category, name,
                    args_callback);
}

base::Optional<uint32_t> SliceTracker::Scoped(int64_t timestamp,
                                              int64_t ref,
                                              RefType ref_type,
                                              StringId category,
                                              StringId name,
                                              int64_t duration,
                                              SetArgsCallback args_callback) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    context_->storage->IncrementStats(stats::slice_out_of_order);
    return base::nullopt;
  }
  prev_timestamp_ = timestamp;

  PERFETTO_DCHECK(duration >= 0);
  MaybeCloseStack(timestamp, &stacks_[{ref, ref_type}]);
  return StartSlice(timestamp, duration, ref, ref_type, category, name,
                    args_callback);
}

base::Optional<uint32_t> SliceTracker::StartSlice(
    int64_t timestamp,
    int64_t duration,
    int64_t ref,
    RefType ref_type,
    StringId category,
    StringId name,
    SetArgsCallback args_callback) {
  auto* stack = &stacks_[{ref, ref_type}];
  auto* slices = context_->storage->mutable_nestable_slices();

  const uint8_t depth = static_cast<uint8_t>(stack->size());
  if (depth >= std::numeric_limits<uint8_t>::max()) {
    PERFETTO_DFATAL("Slices with too large depth found.");
    return base::nullopt;
  }
  int64_t parent_stack_id =
      depth == 0 ? 0 : slices->stack_ids()[stack->back().first];
  uint32_t slice_idx =
      slices->AddSlice(timestamp, duration, ref, ref_type, category, name,
                       depth, 0, parent_stack_id);
  stack->emplace_back(std::make_pair(slice_idx, ArgsTracker(context_)));

  if (args_callback) {
    args_callback(
        &stack->back().second,
        TraceStorage::CreateRowId(TableId::kNestableSlices, slice_idx));
  }
  slices->set_stack_id(slice_idx, GetStackHash(*stack));
  return slice_idx;
}

base::Optional<uint32_t> SliceTracker::EndAndroid(int64_t timestamp,
                                                  uint32_t ftrace_tid,
                                                  uint32_t atrace_tgid) {
  auto actual_tgid_it = ftrace_to_atrace_tgid_.find(ftrace_tid);
  if (actual_tgid_it == ftrace_to_atrace_tgid_.end()) {
    // This is possible if we start tracing after a begin slice.
    PERFETTO_DLOG("Unknown tgid for ftrace tid %u", ftrace_tid);
    return base::nullopt;
  }
  uint32_t actual_tgid = actual_tgid_it->second;
  // atrace_tgid can be 0 in older android versions where the end event would
  // not contain the value.
  if (atrace_tgid != 0 && atrace_tgid != actual_tgid) {
    PERFETTO_DLOG("Mismatched atrace pid %u and looked up pid %u", atrace_tgid,
                  actual_tgid);
    context_->storage->IncrementStats(stats::atrace_tgid_mismatch);
  }
  UniqueTid utid =
      context_->process_tracker->UpdateThread(ftrace_tid, actual_tgid);
  return End(timestamp, utid, RefType::kRefUtid);
}

base::Optional<uint32_t> SliceTracker::End(int64_t timestamp,
                                           int64_t ref,
                                           RefType ref_type,
                                           StringId category,
                                           StringId name,
                                           SetArgsCallback args_callback) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    context_->storage->IncrementStats(stats::slice_out_of_order);
    return base::nullopt;
  }
  prev_timestamp_ = timestamp;

  StackMapKey stack_key = {ref, ref_type};
  MaybeCloseStack(timestamp, &stacks_[stack_key]);

  auto& stack = stacks_[stack_key];
  if (stack.empty())
    return base::nullopt;

  auto* slices = context_->storage->mutable_nestable_slices();
  uint32_t slice_idx = stack.back().first;

  // If we are trying to close mismatching slices (e.g., slices that began
  // before tracing started), bail out.
  if (category && slices->categories()[slice_idx] != category)
    return base::nullopt;
  if (name && slices->names()[slice_idx] != name)
    return base::nullopt;

  PERFETTO_DCHECK(slices->durations()[slice_idx] == kPendingDuration);
  slices->set_duration(slice_idx, timestamp - slices->start_ns()[slice_idx]);

  if (args_callback) {
    args_callback(
        &stack.back().second,
        TraceStorage::CreateRowId(TableId::kNestableSlices, slice_idx));
  }

  return CompleteSlice(stack_key);
  // TODO(primiano): auto-close B slices left open at the end.
}

void SliceTracker::FlushPendingSlices() {
  // Clear the remaining stack entries. This ensures that any pending args are
  // written to the storage. We don't close any slices with kPendingDuration so
  // that the UI can still distinguish such "incomplete" slices.
  //
  // TODO(eseckler): Reconsider whether we want to close pending slices by
  // setting their duration to |trace_end - event_start|. Might still want some
  // additional way of flagging these events as "incomplete" to the UI.
  stacks_.clear();
}

base::Optional<uint32_t> SliceTracker::CompleteSlice(StackMapKey stack_key) {
  auto& stack = stacks_[stack_key];
  uint32_t slice_idx = stack.back().first;
  stack.pop_back();
  return slice_idx;
}

void SliceTracker::MaybeCloseStack(int64_t ts, SlicesStack* stack) {
  const auto& slices = context_->storage->nestable_slices();
  bool pending_dur_descendent = false;
  for (int i = static_cast<int>(stack->size()) - 1; i >= 0; i--) {
    uint32_t slice_idx = (*stack)[static_cast<size_t>(i)].first;

    int64_t start_ts = slices.start_ns()[slice_idx];
    int64_t dur = slices.durations()[slice_idx];
    int64_t end_ts = start_ts + dur;
    if (dur == kPendingDuration) {
      pending_dur_descendent = true;
    }

    if (pending_dur_descendent) {
      PERFETTO_DCHECK(ts >= start_ts);
      // Some trace producers emit END events in the wrong order (even after
      // sorting by timestamp), e.g. BEGIN A, BEGIN B, END A, END B. We discard
      // the mismatching END A in End(). Because of this, we can end up in a
      // situation where we attempt to close the stack on top of A at a
      // timestamp beyond A's parent. To avoid crashing in such a case, we just
      // emit a warning instead.
      if (dur != kPendingDuration && ts > end_ts) {
        PERFETTO_DLOG(
            "Incorrect ordering of begin/end slice events around timestamp "
            "%" PRId64,
            ts);
      }
      continue;
    }

    if (end_ts <= ts) {
      stack->pop_back();
    }
  }
}

int64_t SliceTracker::GetStackHash(const SlicesStack& stack) {
  PERFETTO_DCHECK(!stack.empty());

  const auto& slices = context_->storage->nestable_slices();

  std::string s;
  s.reserve(stack.size() * sizeof(uint64_t) * 2);
  for (size_t i = 0; i < stack.size(); i++) {
    uint32_t slice_idx = stack[i].first;
    s.append(reinterpret_cast<const char*>(&slices.categories()[slice_idx]),
             sizeof(slices.categories()[slice_idx]));
    s.append(reinterpret_cast<const char*>(&slices.names()[slice_idx]),
             sizeof(slices.names()[slice_idx]));
  }
  constexpr uint64_t kMask = uint64_t(-1) >> 1;
  return static_cast<int64_t>((std::hash<std::string>{}(s)) & kMask);
}

}  // namespace trace_processor
}  // namespace perfetto
