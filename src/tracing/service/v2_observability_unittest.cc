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

// Exercise retained metadata through the real service endpoint and consumer.
#include "src/tracing/service/tracing_service_impl.h"

#include "perfetto/tracing/core/data_source_descriptor.h"
#include "protos/perfetto/common/trace_stats.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/test/mock_consumer.h"
#include "src/tracing/test/mock_producer.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {
using testing::_;
using testing::ElementsAre;
using Observation = protos::gen::TraceStats::V2ProducerStats;

class V2ObservabilityTest : public testing::Test {
 protected:
  void SetUp() override {
    service_ = TracingService::CreateInstance(
        std::make_unique<InProcessSharedMemory::Factory>(), &runner_);
    TracingService::ConnectProducerArgs args;
    args.in_process = true;
    args.supports_tracing_v2 = true;
    endpoint_ = service_->ConnectProducer(&producer_, ClientIdentity(0, 0),
                                          "observed", std::move(args));
    runner_.RunUntilIdle();
    DataSourceDescriptor descriptor;
    descriptor.set_name("observed");
    descriptor.set_will_notify_on_stop(false);
    endpoint_->RegisterDataSource(descriptor);
    consumer_.Connect(service_.get());
    ON_CALL(producer_, SetupDataSource(_, _))
        .WillByDefault(
            [this](DataSourceInstanceID, const DataSourceConfig& cfg) {
              targets_.push_back(static_cast<BufferID>(cfg.target_buffer()));
            });
    ON_CALL(producer_, Flush(_, _, _, _))
        .WillByDefault(
            [this](FlushRequestID id, const DataSourceInstanceID*, size_t,
                   FlushFlags) { endpoint_->NotifyFlushComplete(id); });
  }
  void Start(size_t buffers = 1) {
    TraceConfig config;
    for (size_t i = 0; i < buffers; ++i) {
      auto* buffer = config.add_buffers();
      buffer->set_size_kb(64);
      buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
      auto* source = config.add_data_sources()->mutable_config();
      source->set_name("observed");
      source->set_target_buffer(static_cast<uint32_t>(i));
      source->mutable_experimental_tracing_v2()->set_chunk_size_bytes(1024);
    }
    consumer_.EnableTracing(config);
    runner_.RunUntilIdle();
    ASSERT_EQ(targets_.size(), buffers);
  }
  void OfferRingBuffer() {
    auto memory = std::make_unique<InProcessSharedMemory>(1088);
    ring_ = std::make_unique<tracing_v2::SharedRingBuffer>(
        static_cast<uint8_t*>(memory->start()), memory->size(), 256);
    endpoint_->OfferRingBuffer(std::move(memory), 256,
                               [](bool ok) { EXPECT_TRUE(ok); });
  }
  void Publish(BufferID target, WriterID id = 1) {
    auto writer = tracing_v2::test::MakeWriter(ring_.get(), id, target);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, "\x40\x2a"));
  }
  void Drain() {
    endpoint_->DrainRingBuffer();
    runner_.RunUntilIdle();
  }
  TraceStats Stats(MockConsumer& consumer) {
    consumer.GetTraceStats();
    return consumer.WaitForTraceStats(true);
  }
  std::vector<Observation> Read(MockConsumer& consumer) {
    std::vector<Observation> result;
    for (const auto& packet : consumer.ReadBuffers()) {
      if (packet.has_trace_config()) {
        for (const auto& source : packet.trace_config().data_sources())
          EXPECT_EQ(
              source.config().experimental_tracing_v2().chunk_size_bytes(),
              1024u);
      }
      for (const auto& entry : packet.trace_stats().v2_producer_stats())
        result.push_back(entry);
    }
    return result;
  }

  base::TestTaskRunner runner_;
  std::unique_ptr<TracingService> service_;
  testing::NiceMock<MockProducer> producer_{&runner_};
  testing::NiceMock<MockConsumer> consumer_{&runner_};
  std::unique_ptr<ProducerEndpoint> endpoint_;
  std::unique_ptr<tracing_v2::SharedRingBuffer> ring_;
  std::vector<BufferID> targets_;
};

TEST_F(V2ObservabilityTest, LateObservationQueryAndOnceOnlyEmission) {
  Start(2);
  OfferRingBuffer();
  EXPECT_TRUE(Read(consumer_).empty());
  EXPECT_TRUE(Stats(consumer_).v2_producer_stats().empty());
  Publish(targets_[0]);
  Drain();
  auto queried = Stats(consumer_);
  ASSERT_EQ(queried.v2_producer_stats().size(), 1u);
  const auto& entry = queried.v2_producer_stats()[0];
  EXPECT_EQ(entry.accepted_abi_version(), 1u);
  EXPECT_EQ(entry.accepted_chunk_size(), 256u);
  EXPECT_EQ(entry.accepted_num_chunks(), 4u);
  EXPECT_EQ(entry.accepted_ring_size_bytes(), 1088u);
  EXPECT_THAT(entry.observed_target_buffers(), ElementsAre(targets_[0]));
  auto emitted = Read(consumer_);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_THAT(emitted[0].observed_target_buffers(), ElementsAre(targets_[0]));
  Publish(targets_[1], 2);
  Drain();
  auto later = Stats(consumer_);
  ASSERT_EQ(later.v2_producer_stats().size(), 1u);
  EXPECT_THAT(later.v2_producer_stats()[0].observed_target_buffers(),
              ElementsAre(targets_[0], targets_[1]));
  ASSERT_TRUE(consumer_.Flush().WaitForReply());
  // Request another ordinary stats packet through the existing periodic task.
  runner_.AdvanceTimeAndRunUntilIdle(10001);
  EXPECT_TRUE(Read(consumer_).empty());
}

TEST_F(V2ObservabilityTest, LaterSessionReusesAcceptedLayout) {
  Start();
  OfferRingBuffer();
  Publish(targets_[0]);
  Drain();
  auto first = Read(consumer_);
  ASSERT_EQ(first.size(), 1u);
  testing::NiceMock<MockConsumer> later(&runner_);
  later.Connect(service_.get());
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* source = config.add_data_sources()->mutable_config();
  source->set_name("observed");
  source->mutable_experimental_tracing_v2()->set_chunk_size_bytes(512);
  later.EnableTracing(config);
  runner_.RunUntilIdle();
  ASSERT_EQ(targets_.size(), 2u);
  EXPECT_TRUE(Stats(later).v2_producer_stats().empty());
  Publish(targets_[1], 2);
  Drain();
  auto stats = Stats(later);
  ASSERT_EQ(stats.v2_producer_stats().size(), 1u);
  EXPECT_EQ(stats.v2_producer_stats()[0].producer_id(), first[0].producer_id());
  EXPECT_EQ(stats.v2_producer_stats()[0].accepted_chunk_size(), 256u);
  EXPECT_EQ(stats.v2_producer_stats()[0].accepted_ring_size_bytes(), 1088u);
  bool saw_config = false;
  size_t observations = 0;
  for (const auto& packet : later.ReadBuffers()) {
    if (packet.has_trace_config()) {
      saw_config = true;
      EXPECT_EQ(packet.trace_config()
                    .data_sources()[0]
                    .config()
                    .experimental_tracing_v2()
                    .chunk_size_bytes(),
                512u);
    }
    observations += packet.trace_stats().v2_producer_stats().size();
  }
  EXPECT_TRUE(saw_config);
  EXPECT_EQ(observations, 1u);
}

TEST_F(V2ObservabilityTest, UnauthorizedInputDoesNotObserveUse) {
  Start();
  OfferRingBuffer();
  Publish(0);
  Drain();
  EXPECT_TRUE(Stats(consumer_).v2_producer_stats().empty());
  EXPECT_TRUE(Read(consumer_).empty());
  Publish(targets_[0], 2);
  Drain();
  EXPECT_EQ(Read(consumer_).size(), 1u);
}

TEST_F(V2ObservabilityTest, FinalDisconnectDrainRetainsObservation) {
  Start();
  OfferRingBuffer();
  Publish(targets_[0]);
  // Destroying the endpoint runs the service-side disconnect path, which
  // drains the ring and snapshots the observation. Calling Disconnect()
  // directly aborts because the service tears its endpoints down itself.
  endpoint_.reset();
  auto retained = Stats(consumer_);
  ASSERT_EQ(retained.v2_producer_stats().size(), 1u);
  EXPECT_EQ(retained.v2_producer_stats()[0].chunks_read(), 1u);
  EXPECT_EQ(retained.v2_producer_stats()[0].accepted_ring_size_bytes(), 1088u);
  size_t observations = 0;
  size_t recovered = 0;
  for (const auto& packet : consumer_.ReadBuffers()) {
    observations += packet.trace_stats().v2_producer_stats().size();
    if (packet.has_timestamp() && packet.timestamp() == 42)
      ++recovered;
  }
  EXPECT_EQ(observations, 1u);
  EXPECT_EQ(recovered, 1u);
  EXPECT_TRUE(Read(consumer_).empty());
}

TEST_F(V2ObservabilityTest, CloneFreezesCountersAndHistoricalDestinations) {
  Start();
  OfferRingBuffer();
  Publish(targets_[0]);
  Drain();
  EXPECT_EQ(Read(consumer_).size(), 1u);
  testing::NiceMock<MockConsumer> clone(&runner_);
  clone.Connect(service_.get());
  auto cloned = runner_.CreateCheckpoint("cloned");
  EXPECT_CALL(clone, OnSessionCloned(_)).WillOnce([cloned](const auto& result) {
    EXPECT_TRUE(result.success);
    cloned();
  });
  clone.CloneSession(1);
  runner_.RunUntilCheckpoint("cloned");
  auto before = Stats(clone);
  ASSERT_EQ(before.v2_producer_stats().size(), 1u);
  Publish(targets_[0]);
  Drain();
  EXPECT_GT(Stats(consumer_).v2_producer_stats()[0].chunks_read(),
            before.v2_producer_stats()[0].chunks_read());
  endpoint_.reset();
  auto after = Stats(clone);
  EXPECT_EQ(after.v2_producer_stats()[0].SerializeAsString(),
            before.v2_producer_stats()[0].SerializeAsString());
  auto output = Read(clone);
  ASSERT_EQ(output.size(), 1u);
  EXPECT_THAT(output[0].observed_target_buffers(), ElementsAre(targets_[0]));
  EXPECT_TRUE(Read(clone).empty());
}

}  // namespace
}  // namespace perfetto
