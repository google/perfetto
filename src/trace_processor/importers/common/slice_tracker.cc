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

#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {
namespace {
// Slices which have been opened but haven't been closed yet will be marked
// with this duration placeholder.
constexpr int64_t kPendingDuration = -1;
}  // namespace

SliceTracker::SliceTracker(TraceProcessorContext* context)
    : legacy_unnestable_begin_count_string_id_(
          context->storage->InternString("legacy_unnestable_begin_count")),
      legacy_unnestable_last_begin_ts_string_id_(
          context->storage->InternString("legacy_unnestable_last_begin_ts")),
      context_(context) {}

SliceTracker::~SliceTracker() = default;

base::Optional<uint32_t> SliceTracker::Begin(int64_t timestamp,
                                             TrackId track_id,
                                             StringId category,
                                             StringId name,
                                             SetArgsCallback args_callback) {
  tables::SliceTable::Row row(timestamp, kPendingDuration, track_id, category,
                              name);
  return StartSlice(timestamp, track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_slice_table()->Insert(row).id;
  });
}

void SliceTracker::BeginLegacyUnnestable(tables::SliceTable::Row row,
                                         SetArgsCallback args_callback) {
  // Ensure that the duration is pending for this row.
  // TODO(lalitm): change this to eventually use null instead of -1.
  row.dur = kPendingDuration;

  // Double check that if we've seen this track in the past, it was also
  // marked as unnestable then.
#if PERFETTO_DCHECK_IS_ON()
  auto it = stacks_.find(row.track_id);
  PERFETTO_DCHECK(it == stacks_.end() || it->second.is_legacy_unnestable);
#endif

  // Ensure that StartSlice knows that this track is unnestable.
  stacks_[row.track_id].is_legacy_unnestable = true;

  StartSlice(row.ts, row.track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_slice_table()->Insert(row).id;
  });
}

void SliceTracker::BeginGpu(tables::GpuSliceTable::Row row,
                            SetArgsCallback args_callback) {
  // Ensure that the duration is pending for this row.
  // TODO(lalitm): change this to eventually use null instead of -1.
  row.dur = kPendingDuration;

  StartSlice(row.ts, row.track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_gpu_slice_table()->Insert(row).id;
  });
}

void SliceTracker::BeginFrameEvent(tables::GraphicsFrameSliceTable::Row row,
                                   SetArgsCallback args_callback) {
  // Ensure that the duration is pending for this row.
  // TODO(lalitm): change this to eventually use null instead of -1.
  row.dur = kPendingDuration;

  StartSlice(row.ts, row.track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_graphics_frame_slice_table()
        ->Insert(row)
        .id;
  });
}

base::Optional<uint32_t> SliceTracker::Scoped(int64_t timestamp,
                                              TrackId track_id,
                                              StringId category,
                                              StringId name,
                                              int64_t duration,
                                              SetArgsCallback args_callback) {
  PERFETTO_DCHECK(duration >= 0);

  tables::SliceTable::Row row(timestamp, duration, track_id, category, name);
  return StartSlice(timestamp, track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_slice_table()->Insert(row).id;
  });
}

void SliceTracker::ScopedGpu(const tables::GpuSliceTable::Row& row,
                             SetArgsCallback args_callback) {
  PERFETTO_DCHECK(row.dur >= 0);

  StartSlice(row.ts, TrackId(row.track_id), args_callback, [this, &row]() {
    return context_->storage->mutable_gpu_slice_table()->Insert(row).id;
  });
}

SliceId SliceTracker::ScopedFrameEvent(
    const tables::GraphicsFrameSliceTable::Row& row,
    SetArgsCallback args_callback) {
  PERFETTO_DCHECK(row.dur >= 0);

  SliceId id;
  StartSlice(row.ts, TrackId(row.track_id), args_callback, [this, &row, &id]() {
    id =
        context_->storage->mutable_graphics_frame_slice_table()->Insert(row).id;
    return id;
  });
  return id;
}

base::Optional<uint32_t> SliceTracker::End(int64_t timestamp,
                                           TrackId track_id,
                                           StringId category,
                                           StringId name,
                                           SetArgsCallback args_callback) {
  auto finder = [this, category, name](const SlicesStack& stack) {
    return MatchingIncompleteSliceIndex(stack, name, category);
  };
  auto slice_id = CompleteSlice(timestamp, track_id, args_callback, finder);
  if (!slice_id)
    return base::nullopt;
  return context_->storage->slice_table().id().IndexOf(*slice_id);
}

base::Optional<uint32_t> SliceTracker::AddArgs(TrackId track_id,
                                               StringId category,
                                               StringId name,
                                               SetArgsCallback args_callback) {
  auto it = stacks_.find(track_id);
  if (it == stacks_.end())
    return base::nullopt;

  auto& stack = it->second.slice_stack;
  if (stack.empty())
    return base::nullopt;

  auto* slices = context_->storage->mutable_slice_table();
  base::Optional<uint32_t> stack_idx =
      MatchingIncompleteSliceIndex(stack, name, category);
  if (!stack_idx.has_value())
    return base::nullopt;
  uint32_t slice_idx = stack[*stack_idx].row;
  PERFETTO_DCHECK(slices->dur()[slice_idx] == kPendingDuration);
  // Add args to current pending slice.
  ArgsTracker* tracker = &stack[*stack_idx].args_tracker;
  auto bound_inserter = tracker->AddArgsTo(slices->id()[slice_idx]);
  args_callback(&bound_inserter);
  return slice_idx;
}

base::Optional<SliceId> SliceTracker::EndGpu(int64_t ts,
                                             TrackId t_id,
                                             SetArgsCallback args_callback) {
  return CompleteSlice(ts, t_id, args_callback, [](const SlicesStack& stack) {
    return static_cast<uint32_t>(stack.size() - 1);
  });
}

base::Optional<SliceId> SliceTracker::EndFrameEvent(
    int64_t ts,
    TrackId t_id,
    SetArgsCallback args_callback) {
  return CompleteSlice(ts, t_id, args_callback, [](const SlicesStack& stack) {
    return static_cast<uint32_t>(stack.size() - 1);
  });
}

base::Optional<uint32_t> SliceTracker::StartSlice(
    int64_t timestamp,
    TrackId track_id,
    SetArgsCallback args_callback,
    std::function<SliceId()> inserter) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    context_->storage->IncrementStats(stats::slice_out_of_order);
    return base::nullopt;
  }
  prev_timestamp_ = timestamp;

  auto* track_info = &stacks_[track_id];
  auto* stack = &track_info->slice_stack;

  if (track_info->is_legacy_unnestable) {
    PERFETTO_DCHECK(stack->size() <= 1);

    track_info->legacy_unnestable_begin_count++;
    track_info->legacy_unnestable_last_begin_ts = timestamp;

    // If this is an unnestable track, don't start a new slice if one already
    // exists.
    if (!stack->empty()) {
      return base::nullopt;
    }
  }

  auto* slices = context_->storage->mutable_slice_table();
  MaybeCloseStack(timestamp, stack, track_id);

  const uint8_t depth = static_cast<uint8_t>(stack->size());
  if (depth >= std::numeric_limits<uint8_t>::max()) {
    PERFETTO_DFATAL("Slices with too large depth found.");
    return base::nullopt;
  }
  int64_t parent_stack_id =
      depth == 0 ? 0 : slices->stack_id()[stack->back().row];
  base::Optional<tables::SliceTable::Id> parent_id =
      depth == 0 ? base::nullopt
                 : base::make_optional(slices->id()[stack->back().row]);

  SliceId id = inserter();
  uint32_t slice_idx = *slices->id().IndexOf(id);
  StackPush(track_id, slice_idx);

  // Post fill all the relevant columns. All the other columns should have
  // been filled by the inserter.
  slices->mutable_depth()->Set(slice_idx, depth);
  slices->mutable_parent_stack_id()->Set(slice_idx, parent_stack_id);
  slices->mutable_stack_id()->Set(slice_idx, GetStackHash(*stack));
  if (parent_id)
    slices->mutable_parent_id()->Set(slice_idx, *parent_id);

  if (args_callback) {
    ArgsTracker* tracker = &stack->back().args_tracker;
    auto bound_inserter = tracker->AddArgsTo(id);
    args_callback(&bound_inserter);
  }
  return slice_idx;
}

base::Optional<SliceId> SliceTracker::CompleteSlice(
    int64_t timestamp,
    TrackId track_id,
    SetArgsCallback args_callback,
    std::function<base::Optional<uint32_t>(const SlicesStack&)> finder) {
  // At this stage all events should be globally timestamp ordered.
  if (timestamp < prev_timestamp_) {
    context_->storage->IncrementStats(stats::slice_out_of_order);
    return base::nullopt;
  }
  prev_timestamp_ = timestamp;

  auto it = stacks_.find(track_id);
  if (it == stacks_.end())
    return base::nullopt;

  TrackInfo& track_info = it->second;
  SlicesStack& stack = track_info.slice_stack;
  MaybeCloseStack(timestamp, &stack, track_id);
  if (stack.empty())
    return base::nullopt;

  auto* slices = context_->storage->mutable_slice_table();
  base::Optional<uint32_t> stack_idx = finder(stack);

  // If we are trying to close slices that are not open on the stack (e.g.,
  // slices that began before tracing started), bail out.
  if (!stack_idx)
    return base::nullopt;

  const auto& slice_info = stack[stack_idx.value()];
  uint32_t slice_idx = slice_info.row;
  PERFETTO_DCHECK(slices->dur()[slice_idx] == kPendingDuration);
  slices->mutable_dur()->Set(slice_idx, timestamp - slices->ts()[slice_idx]);

  ArgsTracker* tracker = &stack[stack_idx.value()].args_tracker;
  if (args_callback) {
    auto bound_inserter = tracker->AddArgsTo(slices->id()[slice_idx]);
    args_callback(&bound_inserter);
  }

  // Add the legacy unnestable args if they exist.
  if (track_info.is_legacy_unnestable) {
    auto bound_inserter = tracker->AddArgsTo(slices->id()[slice_idx]);
    bound_inserter.AddArg(
        legacy_unnestable_begin_count_string_id_,
        Variadic::Integer(track_info.legacy_unnestable_begin_count));
    bound_inserter.AddArg(
        legacy_unnestable_last_begin_ts_string_id_,
        Variadic::Integer(track_info.legacy_unnestable_last_begin_ts));
  }

  // If this slice is the top slice on the stack, pop it off.
  if (*stack_idx == stack.size() - 1)
    stack.pop_back();
  return slices->id()[slice_idx];
}

// Returns the first incomplete slice in the stack with matching name and
// category. We assume null category/name matches everything. Returns
// nullopt if no matching slice is found.
base::Optional<uint32_t> SliceTracker::MatchingIncompleteSliceIndex(
    const SlicesStack& stack,
    StringId name,
    StringId category) {
  auto* slices = context_->storage->mutable_slice_table();
  for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
    uint32_t slice_idx = stack[static_cast<size_t>(i)].row;
    if (slices->dur()[slice_idx] != kPendingDuration)
      continue;
    const StringId& other_category = slices->category()[slice_idx];
    if (!category.is_null() &&
        (other_category.is_null() || category != other_category))
      continue;
    const StringId& other_name = slices->name()[slice_idx];
    if (!name.is_null() && !other_name.is_null() && name != other_name)
      continue;
    return static_cast<uint32_t>(i);
  }
  return base::nullopt;
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

void SliceTracker::SetOnSliceBeginCallback(OnSliceBeginCallback callback) {
  on_slice_begin_callback_ = callback;
}

base::Optional<SliceId> SliceTracker::GetTopmostSliceOnTrack(
    TrackId track_id) const {
  const auto iter = stacks_.find(track_id);
  if (iter == stacks_.end())
    return base::nullopt;
  const auto& stack = iter->second.slice_stack;
  if (stack.empty())
    return base::nullopt;
  uint32_t slice_idx = stack.back().row;
  return context_->storage->slice_table().id()[slice_idx];
}

void SliceTracker::MaybeCloseStack(int64_t ts,
                                   SlicesStack* stack,
                                   TrackId track_id) {
  auto* slices = context_->storage->mutable_slice_table();
  bool incomplete_descendent = false;
  for (int i = static_cast<int>(stack->size()) - 1; i >= 0; i--) {
    uint32_t slice_idx = (*stack)[static_cast<size_t>(i)].row;

    int64_t start_ts = slices->ts()[slice_idx];
    int64_t dur = slices->dur()[slice_idx];
    int64_t end_ts = start_ts + dur;
    if (dur == kPendingDuration) {
      incomplete_descendent = true;
      continue;
    }

    if (incomplete_descendent) {
      PERFETTO_DCHECK(ts >= start_ts);

      // Only process slices if the ts is past the end of the slice.
      if (ts <= end_ts)
        continue;

      // This usually happens because we have two slices that are partially
      // overlapping.
      // [  slice  1    ]
      //          [     slice 2     ]
      // This is invalid in chrome and should be fixed. Duration events should
      // either be nested or disjoint, never partially intersecting.
      PERFETTO_DLOG(
          "Incorrect ordering of begin/end slice events around timestamp "
          "%" PRId64,
          ts);
      context_->storage->IncrementStats(stats::misplaced_end_event);

      // Every slice below this one should have a pending duration. Update
      // of them to have the end ts of the current slice and pop them
      // all off.
      for (int j = static_cast<int>(stack->size()) - 1; j > i; --j) {
        uint32_t child_idx = (*stack)[static_cast<size_t>(j)].row;
        PERFETTO_DCHECK(slices->dur()[child_idx] == kPendingDuration);
        slices->mutable_dur()->Set(child_idx, end_ts - slices->ts()[child_idx]);
        StackPop(track_id);
      }

      // Also pop the current row itself and reset the incomplete flag.
      StackPop(track_id);
      incomplete_descendent = false;

      continue;
    }

    if (end_ts <= ts) {
      StackPop(track_id);
    }
  }
}

int64_t SliceTracker::GetStackHash(const SlicesStack& stack) {
  PERFETTO_DCHECK(!stack.empty());

  const auto& slices = context_->storage->slice_table();

  base::Hash hash;
  for (size_t i = 0; i < stack.size(); i++) {
    uint32_t slice_idx = stack[i].row;
    hash.Update(slices.category()[slice_idx]);
    hash.Update(slices.name()[slice_idx]);
  }

  // For clients which don't have an integer type (i.e. Javascript), returning
  // hashes which have the top 11 bits set leads to numbers which are
  // unrepresenatble. This means that clients cannot filter using this number as
  // it will be meaningless when passed back to us. For this reason, make sure
  // that the hash is always less than 2^53 - 1.
  constexpr uint64_t kSafeBitmask = (1ull << 53) - 1;
  return static_cast<int64_t>(hash.digest() & kSafeBitmask);
}

void SliceTracker::StackPop(TrackId track_id) {
  stacks_[track_id].slice_stack.pop_back();
}

void SliceTracker::StackPush(TrackId track_id, uint32_t slice_idx) {
  stacks_[track_id].slice_stack.push_back(
      SliceInfo{slice_idx, ArgsTracker(context_)});

  const auto& slices = context_->storage->slice_table();
  if (on_slice_begin_callback_) {
    on_slice_begin_callback_(track_id, slices.id()[slice_idx]);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
