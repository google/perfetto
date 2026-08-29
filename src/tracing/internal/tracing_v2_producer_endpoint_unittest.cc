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
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
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
using ::testing::SaveArg;

// A v1 writer with a real WriterID, which is what the bridge reuses in
// the v2 chunk header. If |deferred_flush_callbacks| is set, Flush() parks its
// callback there, like an unbound SharedMemoryArbiter.
class StubTraceWriter : public TraceWriter {
 public:
  StubTraceWriter(WriterID writer_id,
                  std::vector<std::string>* events,
                  std::vector<std::function<void()>>* deferred_flush_callbacks)
      : writer_id_(writer_id),
        events_(events),
        deferred_flush_callbacks_(deferred_flush_callbacks) {}

  TracePacketHandle NewTracePacket() override {
    FinishTracePacket();
    packet_.Reset();
    packet_open_ = true;
    return TracePacketHandle(packet_.get());
  }
  void FinishTracePacket() override {
    if (!packet_open_)
      return;
    packet_open_ = false;
    packet_->Finalize();
  }
  void Flush(std::function<void()> callback = {}) override {
    FinishTracePacket();
    events_->emplace_back("data");
    if (!callback)
      return;
    if (deferred_flush_callbacks_) {
      deferred_flush_callbacks_->push_back(std::move(callback));
      return;
    }
    callback();
  }
  WriterID writer_id() const override { return writer_id_; }
  uint64_t written() const override { return 0; }
  uint64_t drop_count() const override { return 0; }

 private:
  const WriterID writer_id_;
  std::vector<std::string>* const events_;
  std::vector<std::function<void()>>* const deferred_flush_callbacks_;
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_;
  bool packet_open_ = false;
};

class TracingV2ProducerEndpointTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!tracing_v2::SharedRingBuffer::SupportsWriterWait())
      GTEST_SKIP() << "The tracing v2 ring needs a futex on this platform";

    auto inner = std::unique_ptr<NiceMock<MockProducerEndpoint>>(
        new NiceMock<MockProducerEndpoint>());
    inner_ = inner.get();
    ON_CALL(*inner_, CreateTraceWriter(_, _))
        .WillByDefault(Invoke([this](BufferID, BufferExhaustedPolicy) {
          auto writer = std::unique_ptr<TraceWriter>(new StubTraceWriter(
              next_writer_id_++, &events_,
              defer_flush_callbacks_ ? &deferred_flush_callbacks_ : nullptr));
          last_v1_writer_ = writer.get();
          return writer;
        }));
    endpoint_.reset(new TracingV2ProducerEndpoint(
        std::move(inner), &muxer_task_runner_, &relay_task_runner_));
  }

  void TearDown() override {
    // The same order the muxer uses, and the only safe one: ask for the release
    // first, let the relay run it, and destroy the endpoint - and with it the
    // arbiter the released writers belong to - only afterwards.
    ReleaseV1WritersAndRun();
    endpoint_.reset();
    relay_task_runner_.RunUntilIdle();
    muxer_task_runner_.RunUntilIdle();
  }

  void ReleaseV1WritersAndRun() {
    if (!endpoint_)
      return;
    endpoint_->StartReleasingV1Writers();
    relay_task_runner_.RunUntilIdle();
    EXPECT_FALSE(endpoint_->HasRetainedV1Writers());
  }

  base::TestTaskRunner muxer_task_runner_;
  base::TestTaskRunner relay_task_runner_;
  NiceMock<MockProducerEndpoint>* inner_ = nullptr;
  std::unique_ptr<TracingV2ProducerEndpoint> endpoint_;
  WriterID next_writer_id_ = 1;
  TraceWriter* last_v1_writer_ = nullptr;
  std::vector<std::string> events_;
  // Set before creating a writer; its flushes then wait for
  // AcknowledgeDeferredFlushes().
  bool defer_flush_callbacks_ = false;
  std::vector<std::function<void()>> deferred_flush_callbacks_;

  void AcknowledgeDeferredFlushes() {
    std::vector<std::function<void()>> callbacks;
    callbacks.swap(deferred_flush_callbacks_);
    for (std::function<void()>& callback : callbacks)
      callback();
  }
};

TEST_F(TracingV2ProducerEndpointTest, OtherCallsAreForwardedVerbatim) {
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

  EXPECT_CALL(*inner_, NotifyDataSourceStarted(8));
  endpoint_->NotifyDataSourceStarted(8);

  EXPECT_CALL(*inner_, ActivateTriggers(_));
  endpoint_->ActivateTriggers({"t"});
}

TEST_F(TracingV2ProducerEndpointTest, ControlPlaneBarriersWaitForRingData) {
  auto write_packet = [this](uint64_t timestamp) {
    std::unique_ptr<TraceWriter> writer =
        endpoint_->CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
    writer->NewTracePacket()->set_timestamp(timestamp);
    writer->FinishTracePacket();
  };
  auto expect_after_ring_data = [this](const std::string& control) {
    EXPECT_TRUE(events_.empty());
    muxer_task_runner_.RunUntilIdle();
    EXPECT_TRUE(events_.empty());
    relay_task_runner_.RunUntilIdle();
    ASSERT_EQ(events_.size(), 1u);
    EXPECT_EQ(events_[0], "data");
    muxer_task_runner_.RunUntilIdle();
    ASSERT_EQ(events_.size(), 2u);
    EXPECT_EQ(events_[1], control);
    events_.clear();
  };

  write_packet(1);
  EXPECT_CALL(*inner_, NotifyFlushComplete(7)).WillOnce(Invoke([this] {
    events_.emplace_back("flush complete");
  }));
  endpoint_->NotifyFlushComplete(7);
  expect_after_ring_data("flush complete");

  write_packet(2);
  EXPECT_CALL(*inner_, NotifyDataSourceStopped(9)).WillOnce(Invoke([this] {
    events_.emplace_back("data source stopped");
  }));
  endpoint_->NotifyDataSourceStopped(9);
  expect_after_ring_data("data source stopped");

  write_packet(3);
  bool sync_complete = false;
  EXPECT_CALL(*inner_, Sync(_)).WillOnce(Invoke([this](auto callback) {
    events_.emplace_back("sync");
    callback();
  }));
  endpoint_->Sync([&sync_complete] { sync_complete = true; });
  EXPECT_FALSE(sync_complete);
  expect_after_ring_data("sync");
  EXPECT_TRUE(sync_complete);
}

// An unbound v1 arbiter parks the flush callback until it binds. The control
// request must wait with it.
TEST_F(TracingV2ProducerEndpointTest,
       ControlRequestWaitsForDeferredV1FlushAcknowledgement) {
  defer_flush_callbacks_ = true;
  std::unique_ptr<TraceWriter> writer =
      endpoint_->CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  EXPECT_CALL(*inner_, NotifyFlushComplete(7)).WillOnce(Invoke([this] {
    events_.emplace_back("flush complete");
  }));
  endpoint_->NotifyFlushComplete(7);
  relay_task_runner_.RunUntilIdle();
  muxer_task_runner_.RunUntilIdle();
  relay_task_runner_.RunUntilIdle();
  // The relay flushed the v1 writer, but nothing acknowledged it yet.
  EXPECT_EQ(events_, (std::vector<std::string>{"data"}));
  ASSERT_EQ(deferred_flush_callbacks_.size(), 1u);

  AcknowledgeDeferredFlushes();
  relay_task_runner_.RunUntilIdle();
  muxer_task_runner_.RunUntilIdle();
  EXPECT_EQ(events_, (std::vector<std::string>{"data", "flush complete"}));

  writer.reset();
  relay_task_runner_.RunUntilIdle();
  AcknowledgeDeferredFlushes();
  relay_task_runner_.RunUntilIdle();
}

// A Sync() caller must still be answered, once, after the endpoint is gone.
TEST_F(TracingV2ProducerEndpointTest, SyncCompletesLocallyIfTheEndpointIsGone) {
  EXPECT_CALL(*inner_, Sync(_)).Times(0);
  uint32_t completions = 0;
  endpoint_->Sync([&completions] { ++completions; });

  // The relay resolves the barrier and posts the request to the muxer...
  relay_task_runner_.RunUntilIdle();
  EXPECT_EQ(completions, 0u);

  // ...which destroys the endpoint before that task runs.
  ReleaseV1WritersAndRun();
  endpoint_.reset();
  muxer_task_runner_.RunUntilIdle();
  EXPECT_EQ(completions, 1u);

  relay_task_runner_.RunUntilIdle();
  muxer_task_runner_.RunUntilIdle();
  EXPECT_EQ(completions, 1u);
}

// The service scrapes a no_flush data source instead of flushing it. The
// scrape cannot see the ring, so tell the service the data source needs a
// flush.
TEST_F(TracingV2ProducerEndpointTest,
       NoFlushIsClearedForTheServiceWhileTheRingIsInTheWay) {
  DataSourceDescriptor dsd;
  dsd.set_name("ds");
  dsd.set_no_flush(true);

  DataSourceDescriptor registered;
  EXPECT_CALL(*inner_, RegisterDataSource(_)).WillOnce(SaveArg<0>(&registered));
  endpoint_->RegisterDataSource(dsd);
  EXPECT_EQ(registered.name(), "ds");
  EXPECT_FALSE(registered.no_flush());

  DataSourceDescriptor updated;
  EXPECT_CALL(*inner_, UpdateDataSource(_)).WillOnce(SaveArg<0>(&updated));
  endpoint_->UpdateDataSource(dsd);
  EXPECT_FALSE(updated.no_flush());

  // The caller's own descriptor is untouched.
  EXPECT_TRUE(dsd.no_flush());
}

TEST_F(TracingV2ProducerEndpointTest, WriterCreationIsRoutedThroughTheRing) {
  std::unique_ptr<TraceWriter> writer =
      endpoint_->CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
  ASSERT_NE(writer, nullptr);
  // The v2 writer reuses the v1 writer's id, which is what keeps the
  // packet sequence identity unchanged.
  EXPECT_EQ(writer->writer_id(), 1);
  EXPECT_NE(writer.get(), last_v1_writer_);

  writer.reset();
  relay_task_runner_.RunUntilIdle();
}

TEST_F(TracingV2ProducerEndpointTest, WriterWithNoIdFallsBackToV1) {
  // The arbiter hands back id 0 when it has run out. There is nothing to put in
  // the chunk header, so the caller must transparently keep the v1 writer.
  next_writer_id_ = 0;
  std::unique_ptr<TraceWriter> writer =
      endpoint_->CreateTraceWriter(11, BufferExhaustedPolicy::kDrop);
  ASSERT_NE(writer, nullptr);
  EXPECT_EQ(writer->writer_id(), 0);
  EXPECT_EQ(writer.get(), last_v1_writer_);
}

TEST_F(TracingV2ProducerEndpointTest, StartupWritersBypassTheRing) {
  // Startup writers are created straight off the arbiter, so they never reach
  // CreateTraceWriter() and stay on v1. All this wrapper does is hand the
  // arbiter through.
  EXPECT_CALL(*inner_, MaybeSharedMemoryArbiter()).WillOnce(Return(nullptr));
  EXPECT_EQ(endpoint_->MaybeSharedMemoryArbiter(), nullptr);
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
