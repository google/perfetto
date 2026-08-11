/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/tracing/internal/tracing_v2_producer_endpoint.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "src/tracing/test/proxy_producer_endpoint.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace internal {
namespace {

// Every interesting event lands here in the order it happened, because the
// contract these tests cover is an order and not a set of final states.
using EventLog = std::vector<std::string>;

// The v1 writer the relay forwards into. Records what the relay did to it.
class RecordingTraceWriter : public TraceWriterForTesting {
 public:
  RecordingTraceWriter(EventLog* log, WriterID writer_id)
      : log_(log), writer_id_(writer_id) {}

  WriterID writer_id() const override { return writer_id_; }

  TracePacketHandle NewTracePacket() override {
    log_->push_back("forward");
    return TraceWriterForTesting::NewTracePacket();
  }

  void Flush(std::function<void()> callback) override {
    log_->push_back("downstream_flush");
    TraceWriterForTesting::Flush(std::move(callback));
  }

 private:
  EventLog* const log_;
  const WriterID writer_id_;
};

// Stands in for the real producer endpoint. ProxyProducerEndpoint with no
// backend already drops every call, so only the three the decorator is
// supposed to defer, plus writer creation, need an override here.
class RecordingProducerEndpoint : public ProxyProducerEndpoint {
 public:
  explicit RecordingProducerEndpoint(EventLog* log) : log_(log) {}

  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID,
      BufferExhaustedPolicy) override {
    return std::unique_ptr<TraceWriter>(
        new RecordingTraceWriter(log_, next_writer_id_++));
  }

  void NotifyFlushComplete(FlushRequestID id) override {
    log_->push_back("flush_complete:" + std::to_string(id));
  }

  void NotifyDataSourceStopped(DataSourceInstanceID id) override {
    log_->push_back("stopped:" + std::to_string(id));
  }

  void Sync(std::function<void()> callback) override {
    log_->push_back("sync");
    if (callback)
      callback();
  }

 private:
  EventLog* const log_;
  WriterID next_writer_id_ = 1;
};

// Two independently pumped sequences, so a test can run one and assert on what
// the other has not done yet. Both live on the test thread, which is what makes
// the ordering assertions deterministic rather than timing-dependent.
class TracingV2ProducerEndpointTest : public testing::Test {
 protected:
  TracingV2ProducerEndpointTest() {
    auto owned_endpoint =
        std::unique_ptr<TracingService::ProducerEndpoint>(underlying_);
    endpoint_.reset(new TracingV2ProducerEndpoint(
        std::move(owned_endpoint), &muxer_runner_, &relay_runner_));
  }

  ~TracingV2ProducerEndpointTest() override {
    writer_.reset();
    endpoint_.reset();
    relay_runner_.RunUntilIdle();
    muxer_runner_.RunUntilIdle();
  }

  void CreateWriterAndPublish(uint64_t timestamp) {
    if (!writer_) {
      writer_ = endpoint_->CreateTraceWriter(/*target_buffer=*/1,
                                             BufferExhaustedPolicy::kDrop);
    }
    writer_->NewTracePacket()->set_timestamp(timestamp);
    writer_->FinishTracePacket();
  }

  EventLog log_;
  RecordingProducerEndpoint* const underlying_ =
      new RecordingProducerEndpoint(&log_);
  base::TestTaskRunner muxer_runner_;
  base::TestTaskRunner relay_runner_;
  std::unique_ptr<TracingV2ProducerEndpoint> endpoint_;
  std::unique_ptr<TraceWriter> writer_;
};

TEST_F(TracingV2ProducerEndpointTest, FlushCompleteWaitsForTheRelay) {
  CreateWriterAndPublish(1);

  endpoint_->NotifyFlushComplete(7);
  // Returned without blocking, and nothing has reached the service: the packet
  // is still in the ring, so the acknowledgement would be a lie. Pumping the
  // muxer alone must not change that - only the relay can.
  EXPECT_TRUE(log_.empty());
  muxer_runner_.RunUntilIdle();
  EXPECT_TRUE(log_.empty());

  relay_runner_.RunUntilIdle();
  EXPECT_THAT(log_, testing::ElementsAre("forward", "downstream_flush"));

  muxer_runner_.RunUntilIdle();
  EXPECT_THAT(log_, testing::ElementsAre("forward", "downstream_flush",
                                         "flush_complete:7"));
}

TEST_F(TracingV2ProducerEndpointTest, DataSourceStoppedWaitsForTheRelay) {
  CreateWriterAndPublish(1);

  endpoint_->NotifyDataSourceStopped(3);
  EXPECT_TRUE(log_.empty());
  muxer_runner_.RunUntilIdle();
  EXPECT_TRUE(log_.empty());

  relay_runner_.RunUntilIdle();
  muxer_runner_.RunUntilIdle();
  EXPECT_THAT(log_,
              testing::ElementsAre("forward", "downstream_flush", "stopped:3"));
}

TEST_F(TracingV2ProducerEndpointTest, SyncWaitsForTheRelay) {
  CreateWriterAndPublish(1);

  bool callback_ran = false;
  endpoint_->Sync([&callback_ran] { callback_ran = true; });
  EXPECT_TRUE(log_.empty());
  EXPECT_FALSE(callback_ran);
  muxer_runner_.RunUntilIdle();
  EXPECT_TRUE(log_.empty());

  relay_runner_.RunUntilIdle();
  EXPECT_FALSE(callback_ran);

  muxer_runner_.RunUntilIdle();
  EXPECT_THAT(log_,
              testing::ElementsAre("forward", "downstream_flush", "sync"));
  EXPECT_TRUE(callback_ran);
}

// The muxer must stay available while a fence is outstanding, because the
// relay can need it: forwarding into a stalling downstream writer waits for
// the muxer to issue the arbiter's pending commits. A blocking barrier fails
// this outright.
TEST_F(TracingV2ProducerEndpointTest, MuxerRemainsRunnableWhileFenceIsPending) {
  CreateWriterAndPublish(1);
  endpoint_->NotifyFlushComplete(7);

  bool unrelated_muxer_work_ran = false;
  muxer_runner_.PostTask(
      [&unrelated_muxer_work_ran] { unrelated_muxer_work_ran = true; });
  muxer_runner_.RunUntilIdle();

  EXPECT_TRUE(unrelated_muxer_work_ran);
  // Still pending, i.e. the muxer really did run while the fence was open.
  EXPECT_TRUE(log_.empty());

  relay_runner_.RunUntilIdle();
  muxer_runner_.RunUntilIdle();
  EXPECT_THAT(log_, testing::ElementsAre("forward", "downstream_flush",
                                         "flush_complete:7"));
}

// Fences are ordered by the two task runners alone, which is why the bridge
// needs no queue of its own. Each acknowledgement must also cover the packets
// published before it.
TEST_F(TracingV2ProducerEndpointTest, FencesReachTheServiceInCallOrder) {
  CreateWriterAndPublish(1);
  endpoint_->NotifyFlushComplete(1);
  CreateWriterAndPublish(2);
  endpoint_->NotifyDataSourceStopped(2);
  CreateWriterAndPublish(3);
  endpoint_->Sync({});

  relay_runner_.RunUntilIdle();
  muxer_runner_.RunUntilIdle();

  EventLog control_only;
  for (const std::string& event : log_) {
    if (event != "forward" && event != "downstream_flush")
      control_only.push_back(event);
  }
  EXPECT_THAT(control_only,
              testing::ElementsAre("flush_complete:1", "stopped:2", "sync"));
  // Every packet reached the downstream writer before the last acknowledgement.
  EXPECT_EQ(std::count(log_.begin(), log_.end(), "forward"), 3);
  EXPECT_EQ(log_.back(), "sync");
}

// A fence outliving its endpoint must become a no-op rather than a call
// through a dangling pointer. The weak pointer is copied on the relay and only
// tested back on the muxer.
TEST_F(TracingV2ProducerEndpointTest, PendingFenceIsCancelledWithTheEndpoint) {
  CreateWriterAndPublish(1);
  bool sync_callback_ran = false;
  endpoint_->Sync([&sync_callback_ran] { sync_callback_ran = true; });

  writer_.reset();
  endpoint_.reset();

  relay_runner_.RunUntilIdle();
  muxer_runner_.RunUntilIdle();

  // No underlying call, and the owned callback was dropped rather than run
  // through a destroyed endpoint.
  EXPECT_EQ(std::count(log_.begin(), log_.end(), "sync"), 0);
  EXPECT_FALSE(sync_callback_ran);
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
