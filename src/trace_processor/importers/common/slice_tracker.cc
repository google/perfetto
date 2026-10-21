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

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

SliceTracker::SliceTracker(TraceProcessorContext* context)
    : legacy_unnestable_begin_count_string_id_(
          context->storage->InternString("legacy_unnestable_begin_count")),
      legacy_unnestable_last_begin_ts_string_id_(
          context->storage->InternString("legacy_unnestable_last_begin_ts")),
      context_(context),
      overlap_start_key_(context->storage->InternString("overlap_start")),
      overlap_end_key_(context->storage->InternString("overlap_end")),
      overlap_conflicting_name_key_(
          context->storage->InternString("conflicting_slice_name")),
      overlap_conflicting_ts_key_(
          context->storage->InternString("conflicting_slice_ts")),
      overlap_conflicting_dur_key_(
          context->storage->InternString("conflicting_slice_dur")),
      max_depth_parent_name_key_(
          context->storage->InternString("parent_slice_name")),
      max_depth_current_name_key_(
          context->storage->InternString("current_slice_name")) {}

void SliceTracker::AddOverlapArgs(const OverlapInfo& info,
                                  ArgsTracker::BoundInserter& inserter) const {
  inserter.AddArg(overlap_start_key_, Variadic::Integer(info.start));
  inserter.AddArg(overlap_end_key_, Variadic::Integer(info.end));
  inserter.AddArg(overlap_conflicting_name_key_,
                  Variadic::String(info.conflicting_name));
  inserter.AddArg(overlap_conflicting_ts_key_,
                  Variadic::Integer(info.conflicting_ts));
  inserter.AddArg(overlap_conflicting_dur_key_,
                  Variadic::Integer(info.conflicting_dur));
}

void SliceTracker::AddMaxDepthArgs(StringId parent_name,
                                   StringId current_name,
                                   ArgsTracker::BoundInserter& inserter) const {
  inserter.AddArg(max_depth_parent_name_key_, Variadic::String(parent_name));
  inserter.AddArg(max_depth_current_name_key_, Variadic::String(current_name));
}

SliceSink::~SliceSink() = default;

SliceTracker::~SliceTracker() {
  FlushPendingSlices();
}

void SliceTracker::SetSliceSink(SliceSink* sink, SliceSink::Mode mode) {
  const bool writes_table = !sink || mode == SliceSink::Mode::kAlongsideTable;
  if (writes_table != WritesTable()) {
    // An open slice holds either a table row or detached args; neither can be
    // converted to the other.
    for (auto it = stacks_.GetIterator(); it; ++it)
      PERFETTO_CHECK(it.value().slice_stack.empty());
  }
  if (sink_) {
    DrainFinalized();
    DeliverUntranslatedRecords();
  }
  sink_ = sink;
  sink_mode_ = mode;
  last_started_.reset();
  last_ended_.reset();
}

void SliceTracker::DrainFinalized() {
  PERFETTO_CHECK(!draining_);
  if (finalized_.empty() && finalized_records_.empty())
    return;
  draining_ = true;
  for (SliceId id : finalized_)
    sink_->OnSliceFinalized(id);
  finalized_.clear();
  for (FinalizedSlice& record : finalized_records_)
    sink_->OnSliceRecord(std::move(record));
  finalized_records_.clear();
  draining_ = false;
}

void SliceTracker::DeliverUntranslatedRecords() {
  PERFETTO_CHECK(!draining_);
  draining_ = true;
  for (FinalizedSlice& record : untranslated_records_) {
    ArgsInserter translated =
        ArgsTracker(context_).AddArgsDetached(record.id.value);
    context_->args_translation_table->TranslateArgs(record.args, translated);
    record.args = std::move(translated).ToCompactArgSet();
    sink_->OnSliceRecord(std::move(record));
  }
  untranslated_records_.clear();
  draining_ = false;
}

void SliceTracker::QueueRecord(SliceInfo& info) {
  const bool translate = info.args && info.args->NeedsTranslation(
                                          *context_->args_translation_table);
  auto& queue = translate ? untranslated_records_ : finalized_records_;
  queue.push_back(FinalizedSlice{
      info.id, info.ts, info.dur, info.track_id, info.category, info.name,
      info.depth, info.parent_id, info.thread, ArgsInserter::CompactArgSet()});
  if (info.args) {
    queue.back().args = std::move(*info.args).ToCompactArgSet();
    info.args.reset();
  }
}

FinalizedSlice* SliceTracker::FindQueuedRecord(SliceId id) {
  for (auto* queue : {&finalized_records_, &untranslated_records_}) {
    for (auto it = queue->rbegin(); it != queue->rend(); ++it) {
      if (it->id == id)
        return &*it;
    }
  }
  return nullptr;
}

void SliceTracker::RecordSliceNegativeDuration(int64_t timestamp) {
  context_->import_logs_tracker->RecordParserLog(stats::slice_negative_duration,
                                                 timestamp);
}

bool SliceTracker::PrepareStartSlice(TrackInfo& track_info,
                                     int64_t timestamp,
                                     int64_t duration,
                                     std::optional<OverlapInfo>* overlap_out) {
  if (track_info.is_legacy_unnestable) {
    PERFETTO_DCHECK(track_info.slice_stack.size() <= 1);

    track_info.legacy_unnestable_begin_count++;
    track_info.legacy_unnestable_last_begin_ts = timestamp;

    // If this is an unnestable track, don't start a new slice if one already
    // exists.
    if (!track_info.slice_stack.empty()) {
      return false;
    }
  }

  return MaybeCloseStack(track_info, timestamp, duration, overlap_out);
}

void SliceTracker::LogMaxDepthExceeded(const SliceInfo& parent,
                                       StringId name,
                                       int64_t timestamp) {
  StringId parent_name_id = parent.name;
  StringId current_name_id = name.is_null() ? kNullStringId : name;

  context_->import_logs_tracker->RecordParserLog(
      stats::slice_max_depth_exceeded, timestamp,
      [this, parent_name_id,
       current_name_id](ArgsTracker::BoundInserter& inserter) {
        AddMaxDepthArgs(parent_name_id, current_name_id, inserter);
      });
}

SliceTracker::StartedSlice SliceTracker::StartSlice(
    int64_t timestamp,
    int64_t duration,
    TrackId track_id,
    StringId category,
    StringId raw_name,
    bool want_args,
    std::optional<OverlapInfo>* overlap_out) {
  DrainFinalized();
  const StringId name =
      context_->slice_translation_table->TranslateName(raw_name);

  // Resolve the track once and thread it through; nothing below rehashes.
  TrackInfo& track_info = GetOrCreateTrackInfo(track_id);
  if (!PrepareStartSlice(track_info, timestamp, duration, overlap_out))
    return {};

  auto& stack = track_info.slice_stack;
  size_t depth = stack.size();
  if (PERFETTO_UNLIKELY(depth >= kMaxDepth)) {
    LogMaxDepthExceeded(stack.back(), name, timestamp);
    return {};
  }

  std::optional<SliceId> parent_id;
  if (depth != 0)
    parent_id = stack.back().id;

  std::optional<tables::SliceTable::RowNumber> row_number;
  SliceId id{0u};
  if (WritesTable()) {
    // Set depth/parent pre-insert so they ride the single table insert.
    tables::SliceTable::Row row(timestamp, duration, track_id, category, name);
    row.depth = static_cast<uint32_t>(depth);
    row.parent_id = parent_id;
    auto inserted =
        context_->storage->mutable_slice_table()->Insert(std::move(row));
    // A row must not reuse an id already issued as detached.
    PERFETTO_CHECK(inserted.id.value >=
                   context_->storage->next_detached_slice_id());
    row_number = inserted.row_number;
    id = inserted.id;
  } else {
    id = context_->storage->NextDetachedSliceId();
  }
  last_started_ = SliceStart{id, timestamp};
  last_started_track_ = track_id;
  StackPush(track_info, track_id,
            SliceInfo{row_number, id, timestamp, duration, track_id, category,
                      name, static_cast<uint32_t>(depth), parent_id,
                      ThreadTiming{}, std::nullopt});

  StartedSlice result;
  result.id = id;
  if (want_args)
    result.inserter = GetArgsInserter(stack.back(), id);
  return result;
}

SliceTracker::EndedSlice SliceTracker::CompleteSliceBegin(int64_t timestamp,
                                                          TrackId track_id,
                                                          StringId category,
                                                          StringId raw_name,
                                                          bool want_args) {
  DrainFinalized();
  const StringId name =
      context_->slice_translation_table->TranslateName(raw_name);

  EndedSlice result;
  auto* it = FindTrackInfo(track_id);
  if (!it)
    return result;

  TrackInfo& track_info = *it;
  auto& stack = track_info.slice_stack;
  if (!MaybeCloseStack(track_info, timestamp, kPendingDuration,
                       /*overlap_out=*/nullptr)) {
    return result;
  }
  if (stack.empty())
    return result;

  std::optional<uint32_t> stack_idx =
      MatchingIncompleteSliceIndex(stack, name, category);

  // If we are trying to close slices that are not open on the stack (e.g.,
  // slices that began before tracing started), bail out.
  if (!stack_idx)
    return result;

  SliceInfo& slice_info = stack[*stack_idx];
  PERFETTO_DCHECK(slice_info.dur == kPendingDuration);
  slice_info.dur = timestamp - slice_info.ts;
  if (slice_info.row) {
    slice_info.row->ToRowReference(context_->storage->mutable_slice_table())
        .set_dur(slice_info.dur);
  }
  last_ended_ = SliceStart{slice_info.id, slice_info.ts};
  if (!WritesTable())
    last_ended_thread_ = slice_info.thread;
  last_ended_track_ = track_id;

  result.id = slice_info.id;
  result.state.track_info = &track_info;
  result.state.stack_idx = *stack_idx;
  if (want_args)
    result.inserter = GetArgsInserter(slice_info, slice_info.id);
  return result;
}

std::optional<uint32_t> SliceTracker::AddArgsImpl(TrackId track_id,
                                                  StringId category,
                                                  StringId name,
                                                  bool want_args,
                                                  ArgsInserter** inserter) {
  DrainFinalized();
  auto* it = FindTrackInfo(track_id);
  if (!it)
    return std::nullopt;

  auto& stack = it->slice_stack;
  if (stack.empty())
    return std::nullopt;

  std::optional<uint32_t> stack_idx =
      MatchingIncompleteSliceIndex(stack, name, category);
  if (!stack_idx)
    return std::nullopt;

  SliceInfo& slice_info = stack[*stack_idx];
  PERFETTO_DCHECK(slice_info.dur == kPendingDuration);

  if (want_args)
    *inserter = GetArgsInserter(slice_info, slice_info.id);
  return slice_info.row ? slice_info.row->row_number() : slice_info.id.value;
}

void SliceTracker::CompleteSliceFinalize(const CompleteSliceState& state) {
  TrackInfo& track_info = *state.track_info;
  auto& stack = track_info.slice_stack;
  SliceInfo& slice_info = stack[state.stack_idx];

  // Add the legacy unnestable args if they exist.
  if (track_info.is_legacy_unnestable)
    AddLegacyUnnestableArgs(slice_info, track_info);

  // If this slice is the top slice on the stack, pop it off.
  if (state.stack_idx == stack.size() - 1)
    StackPop(track_info);
}

ArgsInserter* SliceTracker::GetArgsInserter(SliceInfo& slice_info, SliceId id) {
  // Lazily bind the slice's inserter; reused so all its args merge into one
  // set.
  if (!slice_info.args) {
    slice_info.args = WritesTable()
                          ? ArgsTracker(context_).AddArgsTo(id)
                          : ArgsTracker(context_).AddArgsDetached(id.value);
  }
  return &*slice_info.args;
}

void SliceTracker::AddLegacyUnnestableArgs(SliceInfo& slice_info,
                                           const TrackInfo& track_info) {
  ArgsInserter* inserter = GetArgsInserter(slice_info, slice_info.id);
  inserter->AddArg(legacy_unnestable_begin_count_string_id_,
                   Variadic::Integer(track_info.legacy_unnestable_begin_count));
  inserter->AddArg(
      legacy_unnestable_last_begin_ts_string_id_,
      Variadic::Integer(track_info.legacy_unnestable_last_begin_ts));
}

// Returns the first incomplete slice in the stack with matching name and
// category. We assume null category/name matches everything. Returns
// std::nullopt if no matching slice is found.
std::optional<uint32_t> SliceTracker::MatchingIncompleteSliceIndex(
    const SlicesStack& stack,
    StringId name,
    StringId category) {
  for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
    const SliceInfo& info = stack[static_cast<size_t>(i)];
    if (info.dur != kPendingDuration)
      continue;
    if (!category.is_null() &&
        (info.category.is_null() || category != info.category)) {
      continue;
    }
    if (!name.is_null() && !info.name.is_null() && name != info.name) {
      continue;
    }
    return static_cast<uint32_t>(i);
  }
  return std::nullopt;
}

bool SliceTracker::MaybeAddTranslatableArgs(SliceInfo& slice_info) {
  PERFETTO_DCHECK(slice_info.args);
  PERFETTO_DCHECK(WritesTable());
  if (!slice_info.args->NeedsTranslation(*context_->args_translation_table)) {
    return false;
  }
  translatable_args_.emplace_back(TranslatableArgs{
      slice_info.id, std::move(*slice_info.args).ToCompactArgSet()});
  return true;
}

void SliceTracker::FlushPendingSlices() {
  // Clear the remaining stack entries. This ensures that any pending args are
  // written to the storage. We don't close any slices with kPendingDuration so
  // that the UI can still distinguish such "incomplete" slices.
  //
  // TODO(eseckler): Reconsider whether we want to close pending slices by
  // setting their duration to |trace_end - event_start|. Might still want some
  // additional way of flagging these events as "incomplete" to the UI.

  // Defer translatable args; the rest commit when the stacks are cleared below.
  // Without a table, open slices become records instead, top of stack first.
  // Slices still open keep kPendingDuration either way.
  std::vector<SliceId> still_open;
  for (auto it = stacks_.GetIterator(); it; ++it) {
    auto& stack = it.value().slice_stack;
    if (!WritesTable()) {
      for (auto s = stack.rbegin(); s != stack.rend(); ++s)
        QueueRecord(*s);
      continue;
    }
    // Bottom-up, as the order translated arg sets reach the args table must
    // match upstream; the sink still gets the stack top-down.
    size_t first = still_open.size();
    for (auto& slice_info : stack) {
      if (slice_info.args && MaybeAddTranslatableArgs(slice_info))
        continue;  // Delivered with the other translated slices below.
      if (sink_)
        still_open.push_back(slice_info.id);
    }
    std::reverse(still_open.begin() + static_cast<ptrdiff_t>(first),
                 still_open.end());
  }

  // Translate and flush all pending args.
  for (const auto& translatable_arg : translatable_args_) {
    ArgsTracker args_tracker(context_);
    auto bound_inserter = args_tracker.AddArgsTo(translatable_arg.slice_id);
    context_->args_translation_table->TranslateArgs(
        translatable_arg.compact_arg_set, bound_inserter);
  }

  stacks_.Clear();
  if (sink_) {
    finalized_.insert(finalized_.end(), still_open.begin(), still_open.end());
    for (const auto& translatable_arg : translatable_args_)
      finalized_.push_back(translatable_arg.slice_id);
    DrainFinalized();
    DeliverUntranslatedRecords();
  }
  translatable_args_.clear();
}

void SliceTracker::SetOnSliceBeginCallback(OnSliceBeginCallback callback) {
  on_slice_begin_callback_ = std::move(callback);
}

std::optional<SliceId> SliceTracker::GetTopmostSliceOnTrack(
    TrackId track_id) const {
  const auto* iter = stacks_.Find(track_id);
  if (!iter)
    return std::nullopt;
  const auto& stack = iter->slice_stack;
  if (stack.empty())
    return std::nullopt;
  return stack.back().id;
}

void SliceTracker::SetThreadTiming(SliceId id, const ThreadTiming& timing) {
  if (!timing.ts && !timing.dur && !timing.instruction_count &&
      !timing.instruction_delta) {
    return;
  }
  if (WritesTable()) {
    auto rr = (*context_->storage->mutable_slice_table())[id];
    if (timing.ts)
      rr.set_thread_ts(*timing.ts);
    if (timing.dur)
      rr.set_thread_dur(*timing.dur);
    if (timing.instruction_count)
      rr.set_thread_instruction_count(*timing.instruction_count);
    if (timing.instruction_delta)
      rr.set_thread_instruction_delta(*timing.instruction_delta);
    return;
  }
  PERFETTO_CHECK(last_started_ && last_started_->id == id);
  TrackInfo* track_info = FindTrackInfo(*last_started_track_);
  PERFETTO_CHECK(track_info && !track_info->slice_stack.empty() &&
                 track_info->slice_stack.back().id == id);
  SliceInfo& info = track_info->slice_stack.back();
  if (timing.ts)
    info.thread.ts = timing.ts;
  if (timing.dur)
    info.thread.dur = timing.dur;
  if (timing.instruction_count)
    info.thread.instruction_count = timing.instruction_count;
  if (timing.instruction_delta)
    info.thread.instruction_delta = timing.instruction_delta;
}

std::optional<SliceTracker::ThreadTiming>
SliceTracker::ThreadTimingOfRecentlyEnded(SliceId id) const {
  if (WritesTable()) {
    auto rr = context_->storage->slice_table()[id];
    ThreadTiming timing;
    timing.ts = rr.thread_ts();
    timing.dur = rr.thread_dur();
    timing.instruction_count = rr.thread_instruction_count();
    timing.instruction_delta = rr.thread_instruction_delta();
    return timing;
  }
  if (!last_ended_ || last_ended_->id != id)
    return std::nullopt;
  return last_ended_thread_;
}

void SliceTracker::SetThreadDeltas(SliceId id,
                                   std::optional<int64_t> thread_dur,
                                   std::optional<int64_t> instruction_delta) {
  if (!thread_dur && !instruction_delta)
    return;
  if (WritesTable()) {
    auto rr = (*context_->storage->mutable_slice_table())[id];
    if (thread_dur)
      rr.set_thread_dur(*thread_dur);
    if (instruction_delta)
      rr.set_thread_instruction_delta(*instruction_delta);
    return;
  }
  PERFETTO_CHECK(last_ended_ && last_ended_->id == id);
  // The ended slice is either still on its stack (End closed a slice below the
  // top) or, only if it was popped, awaiting delivery.
  SliceThreadTiming* thread = nullptr;
  if (TrackInfo* track_info = FindTrackInfo(*last_ended_track_)) {
    for (auto& info : track_info->slice_stack) {
      if (info.id == id) {
        thread = &info.thread;
        break;
      }
    }
  }
  if (!thread) {
    if (FinalizedSlice* record = FindQueuedRecord(id))
      thread = &record->thread;
  }
  PERFETTO_CHECK(thread);
  if (thread_dur)
    thread->dur = thread_dur;
  if (instruction_delta)
    thread->instruction_delta = instruction_delta;
}

int64_t SliceTracker::StartTsOf(SliceId id) const {
  if (auto ts = StartOfRecentSlice(id))
    return *ts;
  // Without a table the start of an arbitrary slice is not retained.
  PERFETTO_CHECK(WritesTable());
  return context_->storage->slice_table()[id].ts();
}

bool SliceTracker::SetName(TrackId track_id, SliceId id, StringId name) {
  bool found = false;
  if (TrackInfo* track_info = FindTrackInfo(track_id)) {
    for (auto& info : track_info->slice_stack) {
      if (info.id == id) {
        info.name = name;
        found = true;
        break;
      }
    }
  }
  if (WritesTable()) {
    (*context_->storage->mutable_slice_table())[id].set_name(name);
    return true;
  }
  if (!found) {
    if (FinalizedSlice* record = FindQueuedRecord(id)) {
      record->name = name;
      found = true;
    }
  }
  return found;
}

std::optional<SliceTracker::SliceStart> SliceTracker::GetTopmostOpenSlice(
    TrackId track_id) const {
  const auto* iter = stacks_.Find(track_id);
  if (!iter || iter->slice_stack.empty())
    return std::nullopt;
  const SliceInfo& top = iter->slice_stack.back();
  return SliceStart{top.id, top.ts};
}

bool SliceTracker::MaybeCloseStack(TrackInfo& track_info,
                                   int64_t new_ts,
                                   int64_t new_dur,
                                   std::optional<OverlapInfo>* overlap_out) {
  auto& stack = track_info.slice_stack;
  auto* slices = context_->storage->mutable_slice_table();
  bool incomplete_descendent = false;
  for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
    const SliceInfo& info = stack[static_cast<size_t>(i)];

    int64_t start_ts = info.ts;
    int64_t dur = info.dur;
    int64_t end_ts = start_ts + dur;
    if (dur == kPendingDuration) {
      incomplete_descendent = true;
      continue;
    }

    if (incomplete_descendent) {
      PERFETTO_DCHECK(new_ts >= start_ts);

      // Only process slices if the ts is past the end of the slice.
      if (new_ts <= end_ts)
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
          context_->storage->GetString(info.name).c_str(), start_ts, end_ts,
          new_ts);
      context_->stats_tracker->IncrementStats(stats::misplaced_end_event);

      // Every slice below this one should have a pending duration. Update
      // of them to have the end ts of the current slice and pop them
      // all off.
      for (int j = static_cast<int>(stack.size()) - 1; j > i; --j) {
        SliceInfo& child = stack[static_cast<size_t>(j)];
        PERFETTO_DCHECK(child.dur == kPendingDuration);
        child.dur = end_ts - child.ts;
        if (child.row)
          child.row->ToRowReference(slices).set_dur(child.dur);
        StackPop(track_info);
      }

      // Also pop the current row itself and reset the incomplete flag.
      StackPop(track_info);
      incomplete_descendent = false;

      continue;
    }

    // Slices that have ended before the new slice begins can be popped from the
    // stack.
    bool ends_before = end_ts < new_ts;

    // If a slice ends at exactly the same timestamp as another slice, there are
    // multiple cases to consider:
    // 1) previous is a slice, current is a instant.
    // 2) previous is a slice, current is a slice
    // 3) previous is a instant, current is a slice
    // 4) previous is a instant, current is a instant.
    //
    // In general, we follow the principle of: intervals are closed on left and
    // open on right. For instants, this really means they only "interfere"
    // with other instants.
    //
    // Case 1) we want to pop.
    // Case 2) we want to pop.
    // Case 3) we want to pop.
    // Case 4) we want to keep (instants "stack" on top of each other).
    bool ends_same_and_should_drop =
        end_ts == new_ts && !(dur == 0 && new_dur == 0);

    if (ends_before || ends_same_and_should_drop) {
      StackPop(track_info);
      continue;
    }

    if (new_dur == kPendingDuration) {
      // If we don't have a duration, nothing to close.
      continue;
    }

    // This is a sanity check for invalid nesting. This can happen in cases
    // like the following:
    // [  slice  1    ]
    //          [     slice 2     ]
    // This is invalid stacking by the producer and should be fixed. Duration
    // events should either be nested or disjoint, never partially intersecting.
    if (new_ts < end_ts && new_ts + new_dur > end_ts) {
      // The incoming slice [new_ts, new_ts + new_dur) starts inside the
      // already-open slice [start_ts, end_ts) but ends after it, so the shared
      // (ambiguous) region is [new_ts, end_ts).
      OverlapInfo overlap{new_ts, end_ts, info.name, start_ts, dur};
      if (overlap_out) {
        // The caller wants to recover (e.g. spill onto an overflow track) and
        // will do its own logging; just report the details.
        *overlap_out = overlap;
      } else {
        // Nobody can recover this slice, so drop it but log the offending
        // events (rather than only bumping a stat) so the user can find and fix
        // them.
        context_->import_logs_tracker->RecordParserLog(
            stats::slice_drop_overlapping_complete_event, new_ts,
            [this, overlap](ArgsTracker::BoundInserter& inserter) {
              AddOverlapArgs(overlap, inserter);
            });
      }
      return false;
    }
  }
  return true;
}

void SliceTracker::StackPop(TrackInfo& track_info) {
  auto& stack = track_info.slice_stack;
  SliceInfo& info = stack.back();
  if (!WritesTable()) {
    QueueRecord(info);
    stack.pop_back();
    return;
  }
  bool translating = false;
  if (info.args) {
    // Move translatable args out first (deferred to end-of-trace); reset then
    // commits whatever remains.
    translating = MaybeAddTranslatableArgs(info);
    info.args.reset();
  }
  // A slice awaiting translation is delivered once translated, at flush.
  if (sink_ && !translating)
    finalized_.push_back(info.id);
  stack.pop_back();
}

void SliceTracker::StackPush(TrackInfo& track_info,
                             TrackId track_id,
                             SliceInfo info) {
  SliceId id = info.id;
  track_info.slice_stack.push_back(std::move(info));
  if (on_slice_begin_callback_) {
    on_slice_begin_callback_(track_id, id);
  }
}

}  // namespace perfetto::trace_processor
