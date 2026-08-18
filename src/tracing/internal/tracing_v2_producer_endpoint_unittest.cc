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

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace internal {
namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

// A downstream writer with a real WriterID, which is what the bridge reuses in
// the v2 chunk header.
class StubTraceWriter : public TraceWriter {
 public:
  explicit StubTraceWriter(WriterID writer_id) : writer_id_(writer_id) {}
  TracePacketHandle NewTracePacket() override {
    return TracePacketHandle(nullptr);
  }
  void FinishTracePacket() override {}
  void Flush(std::function<void()> callback = {}) override {
    if (callback)
      callback();
  }
  WriterID writer_id() const override { return writer_id_; }
  uint64_t written() const override { return 0; }
  uint64_t drop_count() const override { return 0; }

 private:
  const WriterID writer_id_;
};

class TracingV2ProducerEndpointTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!tracing_v2::kHasFutex)
      GTEST_SKIP() << "The tracing v2 ring needs a futex on this platform";

    auto inner = std::unique_ptr<NiceMock<MockProducerEndpoint>>(
        new NiceMock<MockProducerEndpoint>());
    inner_ = inner.get();
    ON_CALL(*inner_, CreateTraceWriter(_, _))
        .WillByDefault(Invoke([this](BufferID, BufferExhaustedPolicy) {
          return std::unique_ptr<TraceWriter>(
              new StubTraceWriter(next_writer_id_++));
        }));
    endpoint_.reset(new TracingV2ProducerEndpoint(
        std::move(inner), &muxer_task_runner_, &relay_task_runner_));
  }

  void TearDown() override {
    // The same order the muxer uses, and the only safe one: ask for the release
    // first, let the relay run it, and destroy the endpoint - and with it the
    // arbiter the released writers belong to - only afterwards.
    ReleaseRelayWritersAndRun();
    endpoint_.reset();
    relay_task_runner_.RunUntilIdle();
    muxer_task_runner_.RunUntilIdle();
  }

  void ReleaseRelayWritersAndRun() {
    if (!endpoint_)
      return;
    endpoint_->StartReleasingRelayWriters();
    relay_task_runner_.RunUntilIdle();
    EXPECT_FALSE(endpoint_->relay_retains_writers());
  }

  base::TestTaskRunner muxer_task_runner_;
  base::TestTaskRunner relay_task_runner_;
  NiceMock<MockProducerEndpoint>* inner_ = nullptr;
  std::unique_ptr<TracingV2ProducerEndpoint> endpoint_;
  WriterID next_writer_id_ = 1;
};

TEST_F(TracingV2ProducerEndpointTest, GateIsOffByDefault) {
  // Nothing but the test override or the Android aconfig flag turns the v2 path
  // on, and the flag is a build-time false off Android. Every other test in the
  // repository therefore exercises the unchanged v1 path.
  EXPECT_FALSE(UseTracingV2InProcess());
  SetTracingV2InProcessForTesting(true);
  EXPECT_TRUE(UseTracingV2InProcess());
  SetTracingV2InProcessForTesting(false);
  EXPECT_FALSE(UseTracingV2InProcess());
}

TEST_F(TracingV2ProducerEndpointTest, ControlPlaneCallsAreForwardedVerbatim) {
  // Everything except writer creation is a plain passthrough; nothing is
  // fenced, deferred or reordered. See the ordering limitation in the header.
  DataSourceDescriptor dsd;
  dsd.set_name("ds");
  EXPECT_CALL(*inner_, RegisterDataSource(_));
  endpoint_->RegisterDataSource(dsd);

  EXPECT_CALL(*inner_, UpdateDataSource(_));
  endpoint_->UpdateDataSource(dsd);

  EXPECT_CALL(*inner_, UnregisterDataSource("ds"));
  endpoint_->UnregisterDataSource("ds");

  EXPECT_CALL(*inner_, RegisterTraceWriter(3, 4));
  endpoint_->RegisterTraceWriter(3, 4);

  EXPECT_CALL(*inner_, UnregisterTraceWriter(3));
  endpoint_->UnregisterTraceWriter(3);

  EXPECT_CALL(*inner_, NotifyFlushComplete(7));
  endpoint_->NotifyFlushComplete(7);

  EXPECT_CALL(*inner_, NotifyDataSourceStarted(8));
  endpoint_->NotifyDataSourceStarted(8);

  EXPECT_CALL(*inner_, NotifyDataSourceStopped(9));
  endpoint_->NotifyDataSourceStopped(9);

  EXPECT_CALL(*inner_, ActivateTriggers(_));
  endpoint_->ActivateTriggers({"t"});

  bool synced = false;
  EXPECT_CALL(*inner_, Sync(_)).WillOnce(Invoke([](std::function<void()> cb) {
    cb();
  }));
  endpoint_->Sync([&synced] { synced = true; });
  EXPECT_TRUE(synced);
}

TEST_F(TracingV2ProducerEndpointTest, WriterCreationIsRoutedThroughTheRing) {
  const uint64_t before = GetTracingV2WritersCreatedForTesting();
  std::unique_ptr<TraceWriter> writer =
      endpoint_->CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
  ASSERT_NE(writer, nullptr);
  // The v2 writer reuses the downstream writer's id, which is what keeps the
  // packet sequence identity unchanged.
  EXPECT_EQ(writer->writer_id(), 1);
  EXPECT_EQ(GetTracingV2WritersCreatedForTesting(), before + 1);

  writer.reset();
  relay_task_runner_.RunUntilIdle();
}

TEST_F(TracingV2ProducerEndpointTest, WriterWithNoIdFallsBackToV1) {
  // The arbiter hands back id 0 when it has run out. There is nothing to put in
  // the chunk header, so the caller must transparently keep the v1 writer.
  next_writer_id_ = 0;
  const uint64_t before = GetTracingV2WritersCreatedForTesting();
  std::unique_ptr<TraceWriter> writer =
      endpoint_->CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->writer_id(), 0);
  EXPECT_EQ(GetTracingV2WritersCreatedForTesting(), before);
}

TEST_F(TracingV2ProducerEndpointTest, StartupWritersBypassTheRing) {
  // Startup writers are created straight off the arbiter, so they never reach
  // CreateTraceWriter() and stay on v1. All this decorator does is hand the
  // arbiter through.
  EXPECT_CALL(*inner_, MaybeSharedMemoryArbiter()).WillOnce(Return(nullptr));
  EXPECT_EQ(endpoint_->MaybeSharedMemoryArbiter(), nullptr);
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
