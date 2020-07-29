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

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class ArgsTracker;
class TraceProcessorContext;

class SliceTracker {
 public:
  using SetArgsCallback = std::function<void(ArgsTracker::BoundInserter*)>;
  using OnSliceBeginCallback = std::function<void(TrackId, SliceId)>;

  explicit SliceTracker(TraceProcessorContext*);
  virtual ~SliceTracker();

  // virtual for testing
  virtual base::Optional<uint32_t> Begin(
      int64_t timestamp,
      TrackId track_id,
      StringId category,
      StringId name,
      SetArgsCallback args_callback = SetArgsCallback());

  // Unnestable slices are slices which do not have any concept of nesting so
  // starting a new slice when a slice already exists leads to no new slice
  // being added. The number of times a begin event is seen is tracked as well
  // as the latest time we saw a begin event. For legacy Android use only. See
  // the comment in SystraceParser::ParseSystracePoint for information on why
  // this method exists.
  void BeginLegacyUnnestable(tables::SliceTable::Row row);

  void BeginGpu(tables::GpuSliceTable::Row row,
                SetArgsCallback args_callback = SetArgsCallback());

  void BeginFrameEvent(tables::GraphicsFrameSliceTable::Row row,
                       SetArgsCallback args_callback = SetArgsCallback());

  // virtual for testing
  virtual base::Optional<uint32_t> Scoped(
      int64_t timestamp,
      TrackId track_id,
      StringId category,
      StringId name,
      int64_t duration,
      SetArgsCallback args_callback = SetArgsCallback());

  void ScopedGpu(const tables::GpuSliceTable::Row& row,
                 SetArgsCallback args_callback = SetArgsCallback());

  SliceId ScopedFrameEvent(const tables::GraphicsFrameSliceTable::Row& row,
                           SetArgsCallback args_callback = SetArgsCallback());

  // virtual for testing
  virtual base::Optional<uint32_t> End(
      int64_t timestamp,
      TrackId track_id,
      StringId opt_category = {},
      StringId opt_name = {},
      SetArgsCallback args_callback = SetArgsCallback());

  // Usually args should be added in the Begin or End args_callback but this
  // method is for the situation where new args need to be added to an
  // in-progress slice.
  base::Optional<uint32_t> AddArgs(TrackId track_id,
                                   StringId category,
                                   StringId name,
                                   SetArgsCallback args_callback);

  // TODO(lalitm): eventually this method should become End and End should
  // be renamed EndChrome.
  base::Optional<SliceId> EndGpu(
      int64_t ts,
      TrackId track_id,
      SetArgsCallback args_callback = SetArgsCallback());

  base::Optional<SliceId> EndFrameEvent(
      int64_t ts,
      TrackId track_id,
      SetArgsCallback args_callback = SetArgsCallback());

  void FlushPendingSlices();

  void SetOnSliceBeginCallback(OnSliceBeginCallback callback);

  base::Optional<SliceId> GetTopmostSliceOnTrack(TrackId track_id) const;

 private:
  struct SliceInfo {
    uint32_t row;
    ArgsTracker args_tracker;
  };
  using SlicesStack = std::vector<SliceInfo>;

  struct TrackInfo {
    SlicesStack slice_stack;

    // These field is only valid for legacy unnestable slices.
    bool is_legacy_unnestable = false;
    uint32_t legacy_unnestable_begin_count = 0;
    int64_t legacy_unnestable_last_begin_ts = 0;
  };
  using StackMap = std::unordered_map<TrackId, TrackInfo>;

  base::Optional<uint32_t> StartSlice(int64_t timestamp,
                                      TrackId track_id,
                                      SetArgsCallback args_callback,
                                      std::function<SliceId()> inserter);

  base::Optional<SliceId> CompleteSlice(
      int64_t timestamp,
      TrackId track_id,
      SetArgsCallback args_callback,
      std::function<base::Optional<uint32_t>(const SlicesStack&)> finder);

  void MaybeCloseStack(int64_t end_ts, SlicesStack*, TrackId track_id);

  base::Optional<uint32_t> MatchingIncompleteSliceIndex(
      const SlicesStack& stack,
      StringId name,
      StringId category);

  int64_t GetStackHash(const SlicesStack&);

  void StackPop(TrackId track_id);
  void StackPush(TrackId track_id, uint32_t slice_idx);
  void FlowTrackerUpdate(TrackId track_id);

  OnSliceBeginCallback on_slice_begin_callback_;

  // Timestamp of the previous event. Used to discard events arriving out
  // of order.
  int64_t prev_timestamp_ = 0;

  const StringId legacy_unnestable_begin_count_string_id_;
  const StringId legacy_unnestable_last_begin_ts_string_id_;

  TraceProcessorContext* const context_;
  StackMap stacks_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SLICE_TRACKER_H_
