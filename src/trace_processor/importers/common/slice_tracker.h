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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SLICE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SLICE_TRACKER_H_

#include <stdint.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"

namespace perfetto::trace_processor {

class ArgsTracker;
class TraceProcessorContext;

// Thread-time columns of a slice. Unset fields are left untouched.
struct SliceThreadTiming {
  std::optional<int64_t> ts;
  std::optional<int64_t> dur;
  std::optional<int64_t> instruction_count;
  std::optional<int64_t> instruction_delta;
};

// Everything the slice table would hold for one slice, plus its args inline.
struct FinalizedSlice {
  SliceId id;
  int64_t ts;
  int64_t dur;  // -1 if the slice was still open at end of trace.
  TrackId track_id;
  StringId category;
  StringId name;
  uint32_t depth;
  std::optional<SliceId> parent_id;
  SliceThreadTiming thread;
  ArgsInserter::CompactArgSet args;
};

// Receives each slice once it is final: after it has left the stack and after
// the caller has patched the columns it sets once End() returns (thread_dur,
// thread_instruction_delta, a rename). Delivery is deferred to the next
// SliceTracker operation, or to FlushPendingSlices() at end of trace, for that
// reason. A slice whose args need translation is final only once they are
// translated at end of trace, so it is delivered then.
//
// Delivery order is pop order (children before parents, tracks interleaved),
// not timestamp order; slices still open at end of trace follow the same
// rule. The one exception: at end of trace, slices whose args awaited
// translation arrive after all others. Callbacks must not call back into the
// SliceTracker.
class SliceSink {
 public:
  enum class Mode {
    // The slice table is still built; the sink gets ids of final rows.
    kAlongsideTable,
    // No slice table rows and no global arg sets: the sink gets full records
    // with their args. Ids come from a storage-wide sequence that continues
    // after the slice table's rows. Switching back to writing the table once
    // detached ids exist is fatal, as new rows would reuse those ids.
    kInsteadOfTable,
  };

  virtual ~SliceSink();
  virtual void OnSliceFinalized(SliceId) {}
  virtual void OnSliceRecord(FinalizedSlice&&) {}
};

class SliceTracker {
 public:
  static constexpr uint32_t kMaxDepth = 512;

  using OnSliceBeginCallback = std::function<void(TrackId, SliceId)>;

  // Sentinel default args callback; WantsArgs() compiles arg handling away.
  struct NoArgsCallback {
    void operator()(ArgsTracker::BoundInserter*) const {}
  };

  // Describes a partial overlap detected while adding a scoped slice: the
  // incoming slice partially intersects an already-open slice on the same
  // track, so it can neither nest inside it nor sit after it. Overlapping
  // duration events are out of spec and ambiguous; this captures enough context
  // to point a user at the offending events. See
  // https://github.com/google/perfetto/issues/4280.
  struct OverlapInfo {
    // The half-open interval [start, end) shared by the incoming slice and the
    // already-open slice it conflicts with.
    int64_t start;
    int64_t end;
    // The already-open slice the incoming slice conflicts with.
    StringId conflicting_name;
    int64_t conflicting_ts;
    int64_t conflicting_dur;
  };

  // Writes args describing |info| (the ambiguous overlap interval and the slice
  // it conflicts with) onto |inserter|, using the arg-name keys interned once
  // in the constructor. Shared by the two overlap-logging paths (the "drop"
  // path here and the JSON "spill onto an overflow track" path) so both surface
  // the same queryable context in the TraceImportLogsTable.
  void AddOverlapArgs(const OverlapInfo&, ArgsTracker::BoundInserter&) const;

  // Writes args describing parent and current slice names when max depth is
  // exceeded onto |inserter|.
  void AddMaxDepthArgs(StringId parent_name,
                       StringId current_name,
                       ArgsTracker::BoundInserter&) const;

  explicit SliceTracker(TraceProcessorContext*);
  ~SliceTracker();

  template <typename ArgsCb = NoArgsCallback>
  std::optional<SliceId> Begin(int64_t timestamp,
                               TrackId track_id,
                               StringId category,
                               StringId raw_name,
                               ArgsCb args = {}) {
    StartedSlice s =
        StartSlice(timestamp, kPendingDuration, track_id, category, raw_name,
                   WantsArgs(args), /*overlap_out=*/nullptr);
    InvokeArgs(s.inserter, args);
    return s.id;
  }

  // Unnestable slices are slices which do not have any concept of nesting so
  // starting a new slice when a slice already exists leads to no new slice
  // being added. The number of times a begin event is seen is tracked as well
  // as the latest time we saw a begin event. For legacy Android use only. See
  // the comment in SystraceParser::ParseSystracePoint for information on why
  // this method exists.
  template <typename ArgsCb>
  void BeginLegacyUnnestable(tables::SliceTable::Row row, ArgsCb args) {
#if PERFETTO_DCHECK_IS_ON()
    auto* it = stacks_.Find(row.track_id);
    PERFETTO_DCHECK(!it || it->is_legacy_unnestable);
#endif
    GetOrCreateTrackInfo(row.track_id).is_legacy_unnestable = true;
    StartedSlice s = StartSlice(row.ts, kPendingDuration, row.track_id,
                                kNullStringId, row.name.value_or(kNullStringId),
                                WantsArgs(args), /*overlap_out=*/nullptr);
    InvokeArgs(s.inserter, args);
  }

  // If |overlap_out| is non-null and the slice partially overlaps an open slice
  // on the track, the overlap details are reported via |*overlap_out| (and
  // nullopt is returned) instead of dropping the slice and logging
  // |slice_drop_overlapping_complete_event|, letting the caller recover (e.g.
  // by spilling onto an overflow track). When |overlap_out| is null, the drop
  // is logged with the same overlap details.
  template <typename ArgsCb = NoArgsCallback>
  std::optional<SliceId> Scoped(
      int64_t timestamp,
      TrackId track_id,
      StringId category,
      StringId raw_name,
      int64_t duration,
      ArgsCb args = {},
      std::optional<OverlapInfo>* overlap_out = nullptr) {
    if (duration < 0) {
      RecordSliceNegativeDuration(timestamp);
      return std::nullopt;
    }
    StartedSlice s = StartSlice(timestamp, duration, track_id, category,
                                raw_name, WantsArgs(args), overlap_out);
    InvokeArgs(s.inserter, args);
    return s.id;
  }

  template <typename ArgsCb = NoArgsCallback>
  std::optional<SliceId> End(int64_t timestamp,
                             TrackId track_id,
                             StringId category = {},
                             StringId raw_name = {},
                             ArgsCb args = {}) {
    // Split so args are invoked inline between setting the duration and the
    // pop.
    EndedSlice e = CompleteSliceBegin(timestamp, track_id, category, raw_name,
                                      WantsArgs(args));
    if (!e.id)
      return std::nullopt;
    InvokeArgs(e.inserter, args);
    CompleteSliceFinalize(e.state);
    return e.id;
  }

  // Usually args should be added in the Begin or End args_callback but this
  // method is for the situation where new args need to be added to an
  // in-progress slice. Returns the slice's row number, or its id value when
  // no table is written.
  template <typename ArgsCb>
  std::optional<uint32_t> AddArgs(TrackId track_id,
                                  StringId category,
                                  StringId name,
                                  ArgsCb args) {
    ArgsInserter* inserter = nullptr;
    std::optional<uint32_t> row =
        AddArgsImpl(track_id, category, name, WantsArgs(args), &inserter);
    InvokeArgs(inserter, args);
    return row;
  }

  void FlushPendingSlices();

  void SetOnSliceBeginCallback(OnSliceBeginCallback callback);

  // Not owned; must outlive this tracker or be cleared first. Meant to be set
  // before the first slice (from on_slice_tracker_created). Clearing or
  // replacing it mid-trace delivers queued records to the previous sink at
  // once, translating their args early; kAlongsideTable slices still awaiting
  // translation are announced at flush to whichever sink is set then. Whether
  // the table is written can only change while no slice is open.
  void SetSliceSink(SliceSink* sink,
                    SliceSink::Mode mode = SliceSink::Mode::kAlongsideTable);

  // False when slices go only to a kInsteadOfTable sink: their ids then have
  // no slice table row, and nothing may read or write the table by id.
  bool WritesTable() const {
    return !sink_ || sink_mode_ == SliceSink::Mode::kAlongsideTable;
  }

  std::optional<SliceId> GetTopmostSliceOnTrack(TrackId track_id) const;

  // Renames |id| on |track_id|, which must still be open or be the slice
  // returned by the most recent End. Returns false if it is neither and no
  // table is written (the slice was already delivered under its old name).
  bool SetName(TrackId track_id, SliceId id, StringId name);

  struct SliceStart {
    SliceId id;
    int64_t ts;
  };

  using ThreadTiming = SliceThreadTiming;

  // With a slice table the three calls below act on |id|'s row, so any id
  // may be passed. Without one, |id| must be the slice each of them names.

  // Sets thread timing on |id|, the slice returned by the most recent
  // Begin/Scoped.
  void SetThreadTiming(SliceId id, const ThreadTiming& timing);

  // Thread timing recorded at the start of |id|, the slice returned by the
  // most recent End.
  std::optional<ThreadTiming> ThreadTimingOfRecentlyEnded(SliceId id) const;

  // Sets thread_dur / thread_instruction_delta on |id|, the slice returned by
  // the most recent End. A no-op when both are unset.
  void SetThreadDeltas(SliceId id,
                       std::optional<int64_t> thread_dur,
                       std::optional<int64_t> instruction_delta);

  // The topmost open slice on |track_id| and its start timestamp.
  std::optional<SliceStart> GetTopmostOpenSlice(TrackId track_id) const;

  // Start timestamp of |id| if it is the slice returned by the most recent
  // Begin/Scoped or End. Callers attaching data to the slice they were just
  // handed can use this instead of reading the slice table.
  std::optional<int64_t> StartOfRecentSlice(SliceId id) const {
    if (last_started_ && last_started_->id == id)
      return last_started_->ts;
    if (last_ended_ && last_ended_->id == id)
      return last_ended_->ts;
    return std::nullopt;
  }

  // Start timestamp of |id|: StartOfRecentSlice() if it has it, else the slice
  // table. Fatal when neither can answer (no table is written).
  int64_t StartTsOf(SliceId id) const;

 private:
  // Slices which have been opened but haven't been closed yet will be marked
  // with this duration placeholder.
  static constexpr int64_t kPendingDuration = -1;

  // Everything the stack logic needs about an open slice, so it never reads
  // the slice table. |args| is created lazily if the slice gets args, and
  // committed on pop.
  struct SliceInfo {
    std::optional<tables::SliceTable::RowNumber> row;  // Unset without table.
    SliceId id;
    int64_t ts;
    int64_t dur;
    TrackId track_id;
    StringId category;
    StringId name;
    uint32_t depth;
    std::optional<SliceId> parent_id;
    ThreadTiming thread;
    std::optional<ArgsInserter> args;
  };
  using SlicesStack = std::vector<SliceInfo>;

  struct TrackInfo {
    SlicesStack slice_stack;

    // These field is only valid for legacy unnestable slices.
    bool is_legacy_unnestable = false;
    uint32_t legacy_unnestable_begin_count = 0;
    int64_t legacy_unnestable_last_begin_ts = 0;
  };
  using StackMap = base::FlatHashMap<TrackId, TrackInfo>;

  // Args pending translation.
  struct TranslatableArgs {
    SliceId slice_id;
    ArgsTracker::CompactArgSet compact_arg_set;
  };

  // Carried from CompleteSliceBegin to CompleteSliceFinalize across End()'s
  // args.
  struct CompleteSliceState {
    TrackInfo* track_info;
    uint32_t stack_idx;
  };

  // Return of the out-of-line Begin/End fast paths: the new/closed slice id
  // and, when args were requested, a pointer to the inserter parked in the
  // slice's SliceInfo (null otherwise). The pointer is valid until the next
  // mutation of the slice's stack; callers invoke args on it immediately.
  struct StartedSlice {
    std::optional<SliceId> id;
    ArgsInserter* inserter = nullptr;
  };
  struct EndedSlice {
    std::optional<SliceId> id;
    ArgsInserter* inserter = nullptr;
    CompleteSliceState state;
  };

  // Single out-of-line body for Begin/Scoped: translate, insert and push the
  // slice, and (if want_args) acquire its BoundInserter. The only thing the
  // templated callers inline is the args callback on the returned inserter.
  StartedSlice StartSlice(int64_t timestamp,
                          int64_t duration,
                          TrackId track_id,
                          StringId category,
                          StringId raw_name,
                          bool want_args,
                          std::optional<OverlapInfo>* overlap_out);

  // First half of End(): translate, find the slice and set its duration. The
  // caller invokes args (if any) on the returned inserter, then calls
  // CompleteSliceFinalize before any other op on the track.
  EndedSlice CompleteSliceBegin(int64_t timestamp,
                                TrackId track_id,
                                StringId category,
                                StringId raw_name,
                                bool want_args);

  // Second half of End(): legacy args + pop.
  void CompleteSliceFinalize(const CompleteSliceState& state);

  // Body of AddArgs(): finds the slice and (if want_args) returns its inserter
  // via |inserter| (pointer to the slice's parked inserter, else left null).
  std::optional<uint32_t> AddArgsImpl(TrackId track_id,
                                      StringId category,
                                      StringId name,
                                      bool want_args,
                                      ArgsInserter** inserter);

  // True if |args| should run (false for NoArgsCallback / empty std::function).
  template <typename ArgsCb>
  static bool WantsArgs(const ArgsCb& args) {
    if constexpr (std::is_same_v<ArgsCb, NoArgsCallback>) {
      base::ignore_result(args);
      return false;
    } else if constexpr (std::is_constructible_v<bool, const ArgsCb&>) {
      return static_cast<bool>(args);
    } else {
      base::ignore_result(args);
      return true;
    }
  }

  // Runs |args| on the inserter returned by the out-of-line entrypoints. The
  // only per-callsite code for an arg-bearing slice; nothing for
  // NoArgsCallback.
  template <typename ArgsCb>
  static void InvokeArgs(ArgsInserter* inserter, ArgsCb& args) {
    if constexpr (std::is_same_v<ArgsCb, NoArgsCallback>) {
      base::ignore_result(inserter, args);
    } else if (inserter) {
      args(inserter);
    }
  }

  // Legacy bookkeeping + MaybeCloseStack; false if the slice should be dropped.
  [[nodiscard]] bool PrepareStartSlice(TrackInfo& track_info,
                                       int64_t timestamp,
                                       int64_t duration,
                                       std::optional<OverlapInfo>* overlap_out);

  void LogMaxDepthExceeded(const SliceInfo& parent,
                           StringId name,
                           int64_t timestamp);

  void AddLegacyUnnestableArgs(SliceInfo& slice_info,
                               const TrackInfo& track_info);

  void RecordSliceNegativeDuration(int64_t timestamp);

  [[nodiscard]] bool MaybeCloseStack(TrackInfo& track_info,
                                     int64_t ts,
                                     int64_t dur,
                                     std::optional<OverlapInfo>* overlap_out);

  std::optional<uint32_t> MatchingIncompleteSliceIndex(const SlicesStack& stack,
                                                       StringId name,
                                                       StringId category);

  void StackPop(TrackInfo& track_info);

  // Delivers slices popped by the previous operation to |sink_|.
  void DrainFinalized();

  // Translates the args of |untranslated_records_| and delivers them.
  void DeliverUntranslatedRecords();

  // Moves |info| into a record for kInsteadOfTable delivery, queued for the
  // next DrainFinalized() or, if its args need translation, for end of trace.
  void QueueRecord(SliceInfo& info);

  // The queued, undelivered record for |id|, if any.
  FinalizedSlice* FindQueuedRecord(SliceId id);

  void StackPush(TrackInfo& track_info, TrackId track_id, SliceInfo info);

  // Resolve a track once per event and thread the reference through the call;
  // the map is small and hot so a cross-call last-track cache measured as
  // noise.
  TrackInfo& GetOrCreateTrackInfo(TrackId track_id) {
    return stacks_[track_id];
  }
  TrackInfo* FindTrackInfo(TrackId track_id) { return stacks_.Find(track_id); }

  // Returns the inserter parked in |slice_info|, lazily creating it (bound to
  // |id|) on first use. Valid until the slice's stack is next mutated.
  ArgsInserter* GetArgsInserter(SliceInfo& slice_info, SliceId id);

  // Defers args needing translation to end-of-trace (taking ownership of the
  // arg set) and returns true, else no-op. Requires |slice_info.args|
  // non-null and the table to be written.
  bool MaybeAddTranslatableArgs(SliceInfo& slice_info);

  OnSliceBeginCallback on_slice_begin_callback_;

  const StringId legacy_unnestable_begin_count_string_id_;
  const StringId legacy_unnestable_last_begin_ts_string_id_;

  TraceProcessorContext* const context_;

  // Interned arg-name keys for the overlap import logs (see AddOverlapArgs).
  const StringId overlap_start_key_;
  const StringId overlap_end_key_;
  const StringId overlap_conflicting_name_key_;
  const StringId overlap_conflicting_ts_key_;
  const StringId overlap_conflicting_dur_key_;

  // Interned arg-name keys for max depth exceeded import logs.
  const StringId max_depth_parent_name_key_;
  const StringId max_depth_current_name_key_;

  StackMap stacks_;
  std::vector<TranslatableArgs> translatable_args_;

  SliceSink* sink_ = nullptr;
  SliceSink::Mode sink_mode_ = SliceSink::Mode::kAlongsideTable;
  std::vector<SliceId> finalized_;
  std::vector<FinalizedSlice> finalized_records_;
  // kInsteadOfTable records whose args await end-of-trace translation.
  std::vector<FinalizedSlice> untranslated_records_;
  bool draining_ = false;
  std::optional<TrackId> last_ended_track_;

  std::optional<SliceStart> last_started_;
  std::optional<TrackId> last_started_track_;
  std::optional<SliceStart> last_ended_;
  ThreadTiming last_ended_thread_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SLICE_TRACKER_H_
