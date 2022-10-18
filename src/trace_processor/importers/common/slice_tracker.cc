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

#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

SliceTracker::SliceTracker(TraceProcessorContext* context)
    : legacy_unnestable_begin_count_string_id_(
          context->storage->InternString("legacy_unnestable_begin_count")),
      legacy_unnestable_last_begin_ts_string_id_(
          context->storage->InternString("legacy_unnestable_last_begin_ts")),
      context_(context) {}

SliceTracker::~SliceTracker() = default;

base::Optional<SliceId> SliceTracker::Begin(int64_t timestamp,
                                            TrackId track_id,
                                            StringId category,
                                            StringId raw_name,
                                            SetArgsCallback args_callback) {
  const StringId name =
      context_->slice_translation_table->TranslateName(raw_name);
  tables::SliceTable::Row row(timestamp, kPendingDuration, track_id, category,
                              name);
  return StartSlice(timestamp, track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_slice_table()->Insert(row).id;
  });
}

void SliceTracker::BeginLegacyUnnestable(tables::SliceTable::Row row,
                                         SetArgsCallback args_callback) {
  if (row.name) {
    row.name = context_->slice_translation_table->TranslateName(*row.name);
  }

  // Ensure that the duration is pending for this row.
  // TODO(lalitm): change this to eventually use null instead of -1.
  row.dur = kPendingDuration;

  // Double check that if we've seen this track in the past, it was also
  // marked as unnestable then.
#if PERFETTO_DCHECK_IS_ON()
  auto* it = stacks_.Find(row.track_id);
  PERFETTO_DCHECK(!it || it->is_legacy_unnestable);
#endif

  // Ensure that StartSlice knows that this track is unnestable.
  stacks_[row.track_id].is_legacy_unnestable = true;

  StartSlice(row.ts, row.track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_slice_table()->Insert(row).id;
  });
}

base::Optional<SliceId> SliceTracker::Scoped(int64_t timestamp,
                                             TrackId track_id,
                                             StringId category,
                                             StringId raw_name,
                                             int64_t duration,
                                             SetArgsCallback args_callback) {
  PERFETTO_DCHECK(duration >= 0);

  const StringId name =
      context_->slice_translation_table->TranslateName(raw_name);
  tables::SliceTable::Row row(timestamp, duration, track_id, category, name);
  return StartSlice(timestamp, track_id, args_callback, [this, &row]() {
    return context_->storage->mutable_slice_table()->Insert(row).id;
  });
}

base::Optional<SliceId> SliceTracker::End(int64_t timestamp,
                                          TrackId track_id,
                                          StringId category,
                                          StringId raw_name,
                                          SetArgsCallback args_callback) {
  const StringId name =
      context_->slice_translation_table->TranslateName(raw_name);
  auto finder = [this, category, name](const SlicesStack& stack) {
    return MatchingIncompleteSliceIndex(stack, name, category);
  };
  return CompleteSlice(timestamp, track_id, args_callback, finder);
}

base::Optional<uint32_t> SliceTracker::AddArgs(TrackId track_id,
                                               StringId category,
                                               StringId name,
                                               SetArgsCallback args_callback) {
  auto* it = stacks_.Find(track_id);
  if (!it)
    return base::nullopt;

  auto& stack = it->slice_stack;
  if (stack.empty())
    return base::nullopt;

  auto* slices = context_->storage->mutable_slice_table();
  base::Optional<uint32_t> stack_idx =
      MatchingIncompleteSliceIndex(stack, name, category);
  if (!stack_idx.has_value())
    return base::nullopt;

  tables::SliceTable::RowNumber num = stack[*stack_idx].row;
  tables::SliceTable::RowReference ref = num.ToRowReference(slices);
  PERFETTO_DCHECK(ref.dur() == kPendingDuration);

  // Add args to current pending slice.
  ArgsTracker* tracker = &stack[*stack_idx].args_tracker;
  auto bound_inserter = tracker->AddArgsTo(ref.id());
  args_callback(&bound_inserter);
  return num.row_number();
}

base::Optional<SliceId> SliceTracker::StartSlice(
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

  auto& track_info = stacks_[track_id];
  auto& stack = track_info.slice_stack;

  if (track_info.is_legacy_unnestable) {
    PERFETTO_DCHECK(stack.size() <= 1);

    track_info.legacy_unnestable_begin_count++;
    track_info.legacy_unnestable_last_begin_ts = timestamp;

    // If this is an unnestable track, don't start a new slice if one already
    // exists.
    if (!stack.empty()) {
      return base::nullopt;
    }
  }

  auto* slices = context_->storage->mutable_slice_table();
  MaybeCloseStack(timestamp, stack, track_id);

  size_t depth = stack.size();

  base::Optional<tables::SliceTable::RowReference> parent_ref =
      depth == 0 ? base::nullopt
                 : base::make_optional(stack.back().row.ToRowReference(slices));
  int64_t parent_stack_id = parent_ref ? parent_ref->stack_id() : 0;
  base::Optional<tables::SliceTable::Id> parent_id =
      parent_ref ? base::make_optional(parent_ref->id()) : base::nullopt;

  SliceId id = inserter();
  tables::SliceTable::RowReference ref = *slices->FindById(id);
  if (depth >= std::numeric_limits<uint8_t>::max()) {
    auto parent_name = context_->storage->GetString(
        parent_ref->name().value_or(kNullStringId));
    auto name =
        context_->storage->GetString(ref.name().value_or(kNullStringId));
    PERFETTO_DLOG("Last slice: %s", parent_name.c_str());
    PERFETTO_DLOG("Current slice: %s", name.c_str());
    PERFETTO_DFATAL("Slices with too large depth found.");
    return base::nullopt;
  }
  StackPush(track_id, ref);

  // Post fill all the relevant columns. All the other columns should have
  // been filled by the inserter.
  ref.set_depth(static_cast<uint8_t>(depth));
  ref.set_parent_stack_id(parent_stack_id);
  ref.set_stack_id(GetStackHash(stack));
  if (parent_id)
    ref.set_parent_id(*parent_id);

  if (args_callback) {
    auto bound_inserter = stack.back().args_tracker.AddArgsTo(id);
    args_callback(&bound_inserter);
  }
  return id;
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

  auto it = stacks_.Find(track_id);
  if (!it)
    return base::nullopt;

  TrackInfo& track_info = *it;
  SlicesStack& stack = track_info.slice_stack;
  MaybeCloseStack(timestamp, stack, track_id);
  if (stack.empty())
    return base::nullopt;

  auto* slices = context_->storage->mutable_slice_table();
  base::Optional<uint32_t> stack_idx = finder(stack);

  // If we are trying to close slices that are not open on the stack (e.g.,
  // slices that began before tracing started), bail out.
  if (!stack_idx)
    return base::nullopt;

  const auto& slice_info = stack[stack_idx.value()];

  tables::SliceTable::RowReference ref = slice_info.row.ToRowReference(slices);
  PERFETTO_DCHECK(ref.dur() == kPendingDuration);
  ref.set_dur(timestamp - ref.ts());

  ArgsTracker& tracker = stack[stack_idx.value()].args_tracker;
  if (args_callback) {
    auto bound_inserter = tracker.AddArgsTo(ref.id());
    args_callback(&bound_inserter);
  }

  // Add the legacy unnestable args if they exist.
  if (track_info.is_legacy_unnestable) {
    auto bound_inserter = tracker.AddArgsTo(ref.id());
    bound_inserter.AddArg(
        legacy_unnestable_begin_count_string_id_,
        Variadic::Integer(track_info.legacy_unnestable_begin_count));
    bound_inserter.AddArg(
        legacy_unnestable_last_begin_ts_string_id_,
        Variadic::Integer(track_info.legacy_unnestable_last_begin_ts));
  }

  // If this slice is the top slice on the stack, pop it off.
  if (*stack_idx == stack.size() - 1) {
    StackPop(track_id);
  }
  return ref.id();
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
    tables::SliceTable::RowReference ref =
        stack[static_cast<size_t>(i)].row.ToRowReference(slices);
    if (ref.dur() != kPendingDuration)
      continue;
    base::Optional<StringId> other_category = ref.category();
    if (!category.is_null() && (!other_category || other_category->is_null() ||
                                category != other_category)) {
      continue;
    }
    base::Optional<StringId> other_name = ref.name();
    if (!name.is_null() && other_name && !other_name->is_null() &&
        name != other_name) {
      continue;
    }
    return static_cast<uint32_t>(i);
  }
  return base::nullopt;
}

void SliceTracker::MaybeAddTranslatableArgs(SliceInfo& slice_info) {
  if (!slice_info.args_tracker.NeedsTranslation(
          *context_->args_translation_table)) {
    return;
  }
  const auto& table = context_->storage->slice_table();
  tables::SliceTable::ConstRowReference ref =
      slice_info.row.ToRowReference(table);
  translatable_args_.emplace_back(TranslatableArgs{
      ref.id(),
      std::move(slice_info.args_tracker)
          .ToCompactArgSet(table.arg_set_id(), slice_info.row.row_number())});
}

void SliceTracker::FlushPendingSlices() {
  // Clear the remaining stack entries. This ensures that any pending args are
  // written to the storage. We don't close any slices with kPendingDuration so
  // that the UI can still distinguish such "incomplete" slices.
  //
  // TODO(eseckler): Reconsider whether we want to close pending slices by
  // setting their duration to |trace_end - event_start|. Might still want some
  // additional way of flagging these events as "incomplete" to the UI.

  // Make sure that args for all incomplete slice are translated.
  for (auto it = stacks_.GetIterator(); it; ++it) {
    auto& track_info = it.value();
    for (auto& slice_info : track_info.slice_stack) {
      MaybeAddTranslatableArgs(slice_info);
    }
  }

  // Translate and flush all pending args.
  for (const auto& translatable_arg : translatable_args_) {
    auto bound_inserter =
        context_->args_tracker->AddArgsTo(translatable_arg.slice_id);
    context_->args_translation_table->TranslateArgs(
        translatable_arg.compact_arg_set, bound_inserter);
  }
  translatable_args_.clear();

  stacks_.Clear();
}

void SliceTracker::SetOnSliceBeginCallback(OnSliceBeginCallback callback) {
  on_slice_begin_callback_ = callback;
}

base::Optional<SliceId> SliceTracker::GetTopmostSliceOnTrack(
    TrackId track_id) const {
  const auto* iter = stacks_.Find(track_id);
  if (!iter)
    return base::nullopt;
  const auto& stack = iter->slice_stack;
  if (stack.empty())
    return base::nullopt;
  const auto& slice = context_->storage->slice_table();
  return stack.back().row.ToRowReference(slice).id();
}

void SliceTracker::MaybeCloseStack(int64_t ts,
                                   const SlicesStack& stack,
                                   TrackId track_id) {
  auto* slices = context_->storage->mutable_slice_table();
  bool incomplete_descendent = false;
  for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
    tables::SliceTable::RowReference ref =
        stack[static_cast<size_t>(i)].row.ToRowReference(slices);

    int64_t start_ts = ref.ts();
    int64_t dur = ref.dur();
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
      // KI: if tracing both binder and system calls on android, "binder reply"
      // slices will try to escape the enclosing sys_ioctl.
      PERFETTO_DLOG(
          "Incorrect ordering of begin/end slice events. "
          "Truncating incomplete descendants to the end of slice "
          "%s[%" PRId64 ", %" PRId64 "] due to an event at ts=%" PRId64 ".",
          context_->storage->GetString(ref.name().value_or(kNullStringId))
              .c_str(),
          start_ts, end_ts, ts);
      context_->storage->IncrementStats(stats::misplaced_end_event);

      // Every slice below this one should have a pending duration. Update
      // of them to have the end ts of the current slice and pop them
      // all off.
      for (int j = static_cast<int>(stack.size()) - 1; j > i; --j) {
        tables::SliceTable::RowReference child_ref =
            stack[static_cast<size_t>(j)].row.ToRowReference(slices);
        PERFETTO_DCHECK(child_ref.dur() == kPendingDuration);
        child_ref.set_dur(end_ts - child_ref.ts());
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

  base::Hasher hash;
  for (size_t i = 0; i < stack.size(); i++) {
    auto ref = stack[i].row.ToRowReference(slices);
    hash.Update(ref.category().value_or(kNullStringId).raw_id());
    hash.Update(ref.name().value_or(kNullStringId).raw_id());
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
  auto& stack = stacks_[track_id].slice_stack;
  MaybeAddTranslatableArgs(stack.back());
  stack.pop_back();
}

void SliceTracker::StackPush(TrackId track_id,
                             tables::SliceTable::RowReference ref) {
  stacks_[track_id].slice_stack.push_back(
      SliceInfo{ref.ToRowNumber(), ArgsTracker(context_)});
  if (on_slice_begin_callback_) {
    on_slice_begin_callback_(track_id, ref.id());
  }
}

}  // namespace trace_processor
}  // namespace perfetto
