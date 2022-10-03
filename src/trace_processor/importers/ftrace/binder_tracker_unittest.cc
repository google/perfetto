/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/binder_tracker.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {
constexpr int kOneWay = 0x01;

class BinderTrackerTest : public ::testing::Test {
 public:
  BinderTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.global_args_tracker.reset(
        new GlobalArgsTracker(context.storage.get()));
    context.args_tracker.reset(new ArgsTracker(&context));
    context.args_translation_table.reset(
        new ArgsTranslationTable(context.storage.get()));
    context.slice_tracker.reset(new SliceTracker(&context));
    context.slice_translation_table.reset(
        new SliceTranslationTable(context.storage.get()));
    context.process_tracker.reset(new ProcessTracker(&context));
    context.track_tracker.reset(new TrackTracker(&context));
    context.flow_tracker.reset(new FlowTracker(&context));
    binder_tracker = BinderTracker::GetOrCreate(&context);
  }

 protected:
  TraceProcessorContext context;
  BinderTracker* binder_tracker;
};

TEST_F(BinderTrackerTest, RequestReply) {
  int64_t req_ts = 100;
  int64_t req_recv_ts = 105;
  int64_t rep_ts = 150;
  int64_t rep_recv_ts = 155;

  uint32_t req_tid = 5;
  uint32_t rep_tid = 10;

  int32_t req_transaction_id = 1234;
  int32_t rep_transaction_id = 5678;

  binder_tracker->Transaction(req_ts, req_tid, req_transaction_id, 9, rep_tid,
                              rep_tid, false, 0, kNullStringId);
  binder_tracker->TransactionReceived(req_recv_ts, rep_tid, req_transaction_id);

  binder_tracker->Transaction(rep_ts, rep_tid, rep_transaction_id, 99, req_tid,
                              req_tid, true, 0, kNullStringId);
  binder_tracker->TransactionReceived(rep_recv_ts, req_tid, rep_transaction_id);

  const auto& thread = context.storage->thread_table();
  const auto& track = context.storage->thread_track_table();
  const auto& slice = context.storage->slice_table();
  const auto& flow = context.storage->flow_table();
  ASSERT_EQ(slice.row_count(), 2u);

  auto tid_for_slice = [&](uint32_t row) {
    TrackId track_id = slice.track_id()[row];
    UniqueTid utid = track.utid()[*track.id().IndexOf(track_id)];
    return thread.tid()[utid];
  };

  ASSERT_EQ(slice.ts()[0], req_ts);
  ASSERT_EQ(slice.dur()[0], rep_recv_ts - req_ts);
  ASSERT_EQ(tid_for_slice(0), req_tid);

  ASSERT_EQ(slice.ts()[1], req_recv_ts);
  ASSERT_EQ(slice.dur()[1], rep_ts - req_recv_ts);
  ASSERT_EQ(tid_for_slice(1), rep_tid);

  ASSERT_EQ(flow.row_count(), 1u);
  ASSERT_EQ(flow.slice_out()[0], slice.id()[0]);
  ASSERT_EQ(flow.slice_in()[0], slice.id()[1]);
}

TEST_F(BinderTrackerTest, Oneway) {
  int64_t sen_ts = 100;
  int64_t rec_ts = 150;

  uint32_t sen_tid = 5;
  uint32_t rec_tid = 10;

  int32_t transaction_id = 1234;

  binder_tracker->Transaction(sen_ts, sen_tid, transaction_id, 9, rec_tid,
                              rec_tid, false, kOneWay, kNullStringId);
  binder_tracker->TransactionReceived(rec_ts, rec_tid, transaction_id);

  const auto& thread = context.storage->thread_table();
  const auto& track = context.storage->thread_track_table();
  const auto& slice = context.storage->slice_table();
  const auto& flow = context.storage->flow_table();
  ASSERT_EQ(slice.row_count(), 2u);

  auto tid_for_slice = [&](uint32_t row) {
    TrackId track_id = slice.track_id()[row];
    UniqueTid utid = track.utid()[*track.id().IndexOf(track_id)];
    return thread.tid()[utid];
  };

  ASSERT_EQ(slice.ts()[0], sen_ts);
  ASSERT_EQ(slice.dur()[0], 0);
  ASSERT_EQ(tid_for_slice(0), sen_tid);

  ASSERT_EQ(slice.ts()[1], rec_ts);
  ASSERT_EQ(slice.dur()[1], 0);
  ASSERT_EQ(tid_for_slice(1), rec_tid);

  ASSERT_EQ(flow.row_count(), 1u);
  ASSERT_EQ(flow.slice_out()[0], slice.id()[0]);
  ASSERT_EQ(flow.slice_in()[0], slice.id()[1]);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
