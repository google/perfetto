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

#include <cstdint>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
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
    auto rr = track.FindById(slice[row].track_id());
    return thread[rr->utid()].tid();
  };

  ASSERT_EQ(slice[0].ts(), req_ts);
  ASSERT_EQ(slice[0].dur(), rep_recv_ts - req_ts);
  ASSERT_EQ(tid_for_slice(0), req_tid);

  ASSERT_EQ(slice[1].ts(), req_recv_ts);
  ASSERT_EQ(slice[1].dur(), rep_ts - req_recv_ts);
  ASSERT_EQ(tid_for_slice(1), rep_tid);

  ASSERT_EQ(flow.row_count(), 1u);
  ASSERT_EQ(flow[0].slice_out(), slice[0].id());
  ASSERT_EQ(flow[0].slice_in(), slice[1].id());

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
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
    TrackId track_id = slice[row].track_id();
    auto rr = track.FindById(track_id);
    return thread[rr->utid()].tid();
  };

  ASSERT_EQ(slice[0].ts(), sen_ts);
  ASSERT_EQ(slice[0].dur(), 0);
  ASSERT_EQ(tid_for_slice(0), sen_tid);

  ASSERT_EQ(slice[1].ts(), rec_ts);
  ASSERT_EQ(slice[1].dur(), 0);
  ASSERT_EQ(tid_for_slice(1), rec_tid);

  ASSERT_EQ(flow.row_count(), 1u);
  ASSERT_EQ(flow[0].slice_out(), slice[0].id());
  ASSERT_EQ(flow[0].slice_in(), slice[1].id());

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, RequestReplyWithCommands) {
  constexpr uint32_t kSndTid = 5;
  constexpr uint32_t kRcvTid = 10;

  constexpr int32_t kTransactionId = 1234;
  constexpr int32_t kReplyTransactionId = 5678;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->Transaction(ts++, kSndTid, kTransactionId, 9, kRcvTid,
                              kRcvTid, false, 0, kNullStringId);
  binder_tracker->TransactionReceived(ts++, kRcvTid, kTransactionId);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_TRANSACTION);
  binder_tracker->CommandToKernel(ts++, kRcvTid, BinderTracker::kBC_REPLY);
  binder_tracker->Transaction(ts++, kRcvTid, kReplyTransactionId, 99, kSndTid,
                              kSndTid, true, 0, kNullStringId);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_TRANSACTION_COMPLETE);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_TRANSACTION_COMPLETE);
  binder_tracker->TransactionReceived(ts++, kSndTid, kReplyTransactionId);
  binder_tracker->ReturnFromKernel(ts++, kSndTid, BinderTracker::kBR_REPLY);

  const auto& slice = context.storage->slice_table();
  ASSERT_EQ(slice.row_count(), 2u);
  EXPECT_NE(slice[0].dur(), -1);
  EXPECT_NE(slice[1].dur(), -1);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, RequestReplyWithCommandsFailAfterBcTransaction) {
  constexpr uint32_t kSndTid = 5;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_DEAD_REPLY);

  const auto& slice = context.storage->slice_table();
  EXPECT_EQ(slice.row_count(), 0u);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, RequestReplyWithCommandsFailAfterSendTxn) {
  constexpr uint32_t kSndTid = 5;
  constexpr uint32_t kRcvTid = 10;

  constexpr int32_t kTransactionId = 1234;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->Transaction(ts++, kSndTid, kTransactionId, 9, kRcvTid,
                              kRcvTid, false, 0, kNullStringId);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_FAILED_REPLY);

  const auto& slice = context.storage->slice_table();
  ASSERT_EQ(slice.row_count(), 1u);
  EXPECT_NE(slice[0].dur(), -1);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, RequestReplyWithCommandsFailBeforeReplyTxn) {
  constexpr uint32_t kSndTid = 5;
  constexpr uint32_t kRcvTid = 10;

  constexpr int32_t kTransactionId = 1234;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->Transaction(ts++, kSndTid, kTransactionId, 9, kRcvTid,
                              kRcvTid, false, 0, kNullStringId);
  binder_tracker->TransactionReceived(ts++, kRcvTid, kTransactionId);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_TRANSACTION);
  binder_tracker->CommandToKernel(ts++, kRcvTid, BinderTracker::kBC_REPLY);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_FAILED_REPLY);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_TRANSACTION_COMPLETE);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_FAILED_REPLY);

  const auto& slice = context.storage->slice_table();
  ASSERT_EQ(slice.row_count(), 2u);
  EXPECT_NE(slice[0].dur(), -1);
  EXPECT_NE(slice[1].dur(), -1);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, RequestReplyWithCommandsFailAfterReplyTxn) {
  constexpr uint32_t kSndTid = 5;
  constexpr uint32_t kRcvTid = 10;

  constexpr int32_t kTransactionId = 1234;
  constexpr int32_t kReplyTransactionId = 5678;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->Transaction(ts++, kSndTid, kTransactionId, 9, kRcvTid,
                              kRcvTid, false, 0, kNullStringId);
  binder_tracker->TransactionReceived(ts++, kRcvTid, kTransactionId);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_TRANSACTION);
  binder_tracker->CommandToKernel(ts++, kRcvTid, BinderTracker::kBC_REPLY);
  binder_tracker->Transaction(ts++, kRcvTid, kReplyTransactionId, 99, kSndTid,
                              kSndTid, true, 0, kNullStringId);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_TRANSACTION_COMPLETE);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_TRANSACTION_COMPLETE);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_FAILED_REPLY);

  const auto& slice = context.storage->slice_table();
  ASSERT_EQ(slice.row_count(), 2u);
  EXPECT_NE(slice[0].dur(), -1);
  EXPECT_NE(slice[1].dur(), -1);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, OneWayWithCommands) {
  constexpr uint32_t kSndTid = 5;
  constexpr uint32_t kRcvTid = 10;

  constexpr int32_t kTransactionId = 1234;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->Transaction(ts++, kSndTid, kTransactionId, 9, kRcvTid,
                              kRcvTid, false, kOneWay, kNullStringId);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_TRANSACTION_COMPLETE);
  binder_tracker->TransactionReceived(ts++, kRcvTid, kTransactionId);
  binder_tracker->ReturnFromKernel(ts++, kRcvTid,
                                   BinderTracker::kBR_TRANSACTION);

  const auto& slice = context.storage->slice_table();
  ASSERT_EQ(slice.row_count(), 2u);
  EXPECT_EQ(slice[0].dur(), 0);
  EXPECT_EQ(slice[1].dur(), 0);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, OneWayWithCommandsFailBeforeTxn) {
  constexpr uint32_t kSndTid = 5;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_FAILED_REPLY);

  const auto& slice = context.storage->slice_table();
  EXPECT_EQ(slice.row_count(), 0u);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

TEST_F(BinderTrackerTest, OneWayWithCommandsFailAfterTxn) {
  constexpr uint32_t kSndTid = 5;
  constexpr uint32_t kRcvTid = 10;

  constexpr int32_t kTransactionId = 1234;

  int64_t ts = 1;
  binder_tracker->CommandToKernel(ts++, kSndTid,
                                  BinderTracker::kBC_TRANSACTION);
  binder_tracker->Transaction(ts++, kSndTid, kTransactionId, 9, kRcvTid,
                              kRcvTid, false, kOneWay, kNullStringId);
  binder_tracker->ReturnFromKernel(ts++, kSndTid,
                                   BinderTracker::kBR_FAILED_REPLY);

  const auto& slice = context.storage->slice_table();
  ASSERT_EQ(slice.row_count(), 1u);
  EXPECT_EQ(slice[0].dur(), 0);

  EXPECT_TRUE(binder_tracker->utid_stacks_empty());
}

}  // namespace
}  // namespace perfetto::trace_processor
