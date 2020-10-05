/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <vector>

#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::Eq;

TEST(FlowTrackerTest, SingleFlowEventExplicitInSliceBinding) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.slice_tracker.reset(new SliceTracker(&context));
  auto& slice_tracker = context.slice_tracker;
  FlowTracker tracker(&context);
  slice_tracker->SetOnSliceBeginCallback(
      [&tracker](TrackId track_id, SliceId slice_id) {
        tracker.ClosePendingEventsOnTrack(track_id, slice_id);
      });

  FlowId flow_id = 1;
  TrackId track_1(1);
  TrackId track_2(2);

  slice_tracker->Begin(100, track_1, StringId::Raw(1), StringId::Raw(1));
  SliceId out_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.Begin(track_1, flow_id);
  slice_tracker->End(120, track_1, StringId::Raw(1), StringId::Raw(1));

  slice_tracker->Begin(140, track_2, StringId::Raw(2), StringId::Raw(2));
  SliceId in_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_2).value();
  tracker.End(track_2, flow_id, /* bind_enclosing = */ true,
              /* close_flow = */ false);
  slice_tracker->End(160, track_2, StringId::Raw(2), StringId::Raw(2));

  const auto& flows = context.storage->flow_table();
  EXPECT_EQ(flows.row_count(), 1u);
  EXPECT_EQ(flows.slice_out()[0], out_slice_id);
  EXPECT_EQ(flows.slice_in()[0], in_slice_id);
}

TEST(FlowTrackerTest, SingleFlowEventWaitForNextSlice) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.slice_tracker.reset(new SliceTracker(&context));
  auto& slice_tracker = context.slice_tracker;
  FlowTracker tracker(&context);
  slice_tracker->SetOnSliceBeginCallback(
      [&tracker](TrackId track_id, SliceId slice_id) {
        tracker.ClosePendingEventsOnTrack(track_id, slice_id);
      });

  FlowId flow_id = 1;
  TrackId track_1(1);
  TrackId track_2(2);

  slice_tracker->Begin(100, track_1, StringId::Raw(1), StringId::Raw(1));
  SliceId out_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.Begin(track_1, flow_id);
  slice_tracker->End(120, track_1, StringId::Raw(1), StringId::Raw(1));

  tracker.End(track_2, flow_id, /* bind_enclosing = */ false,
              /* close_flow = */ false);

  const auto& flows = context.storage->flow_table();

  EXPECT_EQ(flows.row_count(), 0u);

  slice_tracker->Begin(140, track_2, StringId::Raw(2), StringId::Raw(2));
  SliceId in_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_2).value();
  slice_tracker->End(160, track_2, StringId::Raw(2), StringId::Raw(2));

  EXPECT_EQ(flows.row_count(), 1u);
  EXPECT_EQ(flows.slice_out()[0], out_slice_id);
  EXPECT_EQ(flows.slice_in()[0], in_slice_id);
}

TEST(FlowTrackerTest, SingleFlowEventWaitForNextSliceScoped) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.slice_tracker.reset(new SliceTracker(&context));
  auto& slice_tracker = context.slice_tracker;
  FlowTracker tracker(&context);
  slice_tracker->SetOnSliceBeginCallback(
      [&tracker](TrackId track_id, SliceId slice_id) {
        tracker.ClosePendingEventsOnTrack(track_id, slice_id);
      });

  FlowId flow_id = 1;
  TrackId track_1(1);
  TrackId track_2(2);

  slice_tracker->Begin(100, track_1, StringId::Raw(1), StringId::Raw(1));
  SliceId out_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.Begin(track_1, flow_id);
  slice_tracker->End(120, track_1, StringId::Raw(1), StringId::Raw(1));

  tracker.End(track_2, flow_id, /* bind_enclosing = */ false,
              /* close_flow = */ false);

  const auto& flows = context.storage->flow_table();

  EXPECT_EQ(flows.row_count(), 0u);

  slice_tracker->Scoped(140, track_2, StringId::Raw(2), StringId::Raw(2), 100);
  SliceId in_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_2).value();

  EXPECT_EQ(flows.row_count(), 1u);
  EXPECT_EQ(flows.slice_out()[0], out_slice_id);
  EXPECT_EQ(flows.slice_in()[0], in_slice_id);
}

TEST(FlowTrackerTest, TwoFlowEventsWaitForNextSlice) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.slice_tracker.reset(new SliceTracker(&context));
  auto& slice_tracker = context.slice_tracker;
  FlowTracker tracker(&context);
  slice_tracker->SetOnSliceBeginCallback(
      [&tracker](TrackId track_id, SliceId slice_id) {
        tracker.ClosePendingEventsOnTrack(track_id, slice_id);
      });

  FlowId flow1_id = 1;
  FlowId flow2_id = 2;
  TrackId track_1(1);
  TrackId track_2(2);

  // begin flow1 in enclosing slice1
  slice_tracker->Begin(100, track_1, StringId::Raw(1), StringId::Raw(1));
  SliceId out_slice1_id =
      slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.Begin(track_1, flow1_id);
  tracker.End(track_2, flow1_id, /* bind_enclosing = */ false,
              /* close_flow = */ false);
  slice_tracker->End(120, track_1, StringId::Raw(1), StringId::Raw(1));

  // begin flow2 in enclosing slice2
  slice_tracker->Begin(130, track_1, StringId::Raw(2), StringId::Raw(2));
  SliceId out_slice2_id =
      slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.Begin(track_1, flow2_id);
  tracker.End(track_2, flow2_id, /* bind_enclosing = */ false,
              /* close_flow = */ false);
  slice_tracker->End(140, track_1, StringId::Raw(2), StringId::Raw(2));

  const auto& flows = context.storage->flow_table();

  EXPECT_EQ(flows.row_count(), 0u);

  // close all pending flows
  slice_tracker->Begin(160, track_2, StringId::Raw(3), StringId::Raw(3));
  SliceId in_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_2).value();
  slice_tracker->End(170, track_2, StringId::Raw(3), StringId::Raw(3));

  EXPECT_EQ(flows.row_count(), 2u);
  EXPECT_EQ(flows.slice_out()[0], out_slice1_id);
  EXPECT_EQ(flows.slice_in()[0], in_slice_id);
  EXPECT_EQ(flows.slice_out()[1], out_slice2_id);
  EXPECT_EQ(flows.slice_in()[1], in_slice_id);
}

TEST(FlowTrackerTest, TwoFlowEventsSliceInSlice) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.slice_tracker.reset(new SliceTracker(&context));
  auto& slice_tracker = context.slice_tracker;
  FlowTracker tracker(&context);
  slice_tracker->SetOnSliceBeginCallback(
      [&tracker](TrackId track_id, SliceId slice_id) {
        tracker.ClosePendingEventsOnTrack(track_id, slice_id);
      });

  FlowId flow1_id = 1;
  FlowId flow2_id = 2;
  TrackId track_1(1);
  TrackId track_2(2);

  // start two nested slices
  slice_tracker->Begin(100, track_1, StringId::Raw(1), StringId::Raw(1));
  SliceId out_slice1_id =
      slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  slice_tracker->Begin(120, track_1, StringId::Raw(2), StringId::Raw(2));
  SliceId out_slice2_id =
      slice_tracker->GetTopmostSliceOnTrack(track_1).value();

  tracker.Begin(track_1, flow1_id);

  slice_tracker->End(140, track_1, StringId::Raw(2), StringId::Raw(2));

  tracker.Begin(track_1, flow2_id);

  slice_tracker->End(150, track_1, StringId::Raw(1), StringId::Raw(1));

  slice_tracker->Begin(160, track_2, StringId::Raw(3), StringId::Raw(3));
  SliceId in_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_2).value();

  tracker.End(track_2, flow1_id, /* bind_enclosing = */ true,
              /* close_flow = */ false);
  tracker.End(track_2, flow2_id, /* bind_enclosing = */ true,
              /* close_flow = */ false);

  slice_tracker->End(170, track_2, StringId::Raw(3), StringId::Raw(3));

  const auto& flows = context.storage->flow_table();
  EXPECT_EQ(flows.row_count(), 2u);
  EXPECT_EQ(flows.slice_out()[0], out_slice2_id);
  EXPECT_EQ(flows.slice_in()[0], in_slice_id);
  EXPECT_EQ(flows.slice_out()[1], out_slice1_id);
  EXPECT_EQ(flows.slice_in()[1], in_slice_id);
}

TEST(FlowTrackerTest, FlowEventsWithStep) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.slice_tracker.reset(new SliceTracker(&context));
  auto& slice_tracker = context.slice_tracker;
  FlowTracker tracker(&context);
  slice_tracker->SetOnSliceBeginCallback(
      [&tracker](TrackId track_id, SliceId slice_id) {
        tracker.ClosePendingEventsOnTrack(track_id, slice_id);
      });

  FlowId flow_id = 1;
  TrackId track_1(1);
  TrackId track_2(2);

  // flow begin inside slice1 on track1
  slice_tracker->Begin(100, track_1, StringId::Raw(1), StringId::Raw(1));
  SliceId out_slice1_id =
      slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.Begin(track_1, flow_id);
  slice_tracker->End(140, track_1, StringId::Raw(1), StringId::Raw(1));

  // flow step inside slice2 on track2
  slice_tracker->Begin(160, track_2, StringId::Raw(2), StringId::Raw(2));
  SliceId inout_slice2_id =
      slice_tracker->GetTopmostSliceOnTrack(track_2).value();
  tracker.Step(track_2, flow_id);
  slice_tracker->End(170, track_2, StringId::Raw(2), StringId::Raw(2));

  // flow end inside slice3 on track3
  slice_tracker->Begin(180, track_1, StringId::Raw(3), StringId::Raw(3));
  SliceId in_slice_id = slice_tracker->GetTopmostSliceOnTrack(track_1).value();
  tracker.End(track_1, flow_id, /* bind_enclosing = */ true,
              /* close_flow = */ false);
  slice_tracker->End(190, track_1, StringId::Raw(3), StringId::Raw(3));

  const auto& flows = context.storage->flow_table();
  EXPECT_EQ(flows.row_count(), 2u);
  EXPECT_EQ(flows.slice_out()[0], out_slice1_id);
  EXPECT_EQ(flows.slice_in()[0], inout_slice2_id);
  EXPECT_EQ(flows.slice_out()[1], inout_slice2_id);
  EXPECT_EQ(flows.slice_in()[1], in_slice_id);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
