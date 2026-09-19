/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <cinttypes>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/tracing/core/consumer.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/ipc/consumer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"
#include "src/tracing/ipc/producer/producer_ipc_client_impl.h"
#include "src/tracing/ipc/producer/producer_ipc_client_impl_for_testing.h"
#include "src/tracing/service/tracing_service_impl.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include "src/tracing/ipc/posix_shared_memory.h"
#endif
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/protovm/protovm_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/clock_snapshot.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

using testing::_;
using testing::InvokeWithoutArgs;
using tracing_service::TracingServiceImpl;

ipc::TestSocket kProducerSock{"tracing_test-producer"};
ipc::TestSocket kConsumerSock{"tracing_test-consumer"};

// TODO(rsavitski): consider using src/tracing/test/mock_producer.h.
class MockProducer : public Producer {
 public:
  ~MockProducer() override {}

  // Producer implementation.
  MOCK_METHOD(void, OnConnect, (), (override));
  MOCK_METHOD(void, OnDisconnect, (), (override));
  MOCK_METHOD(void,
              SetupDataSource,
              (DataSourceInstanceID, const DataSourceConfig&),
              (override));
  MOCK_METHOD(void,
              StartDataSource,
              (DataSourceInstanceID, const DataSourceConfig&),
              (override));
  MOCK_METHOD(void, StopDataSource, (DataSourceInstanceID), (override));
  MOCK_METHOD(void, OnTracingSetup, (), (override));
  MOCK_METHOD(void,
              Flush,
              (FlushRequestID, const DataSourceInstanceID*, size_t, FlushFlags),
              (override));
  MOCK_METHOD(void,
              ClearIncrementalState,
              (const DataSourceInstanceID*, size_t),
              (override));
};

class MockConsumer : public Consumer {
 public:
  ~MockConsumer() override {}

  // Producer implementation.
  MOCK_METHOD(void, OnConnect, (), (override));
  MOCK_METHOD(void, OnDisconnect, (), (override));
  MOCK_METHOD(void,
              OnTracingDisabled,
              (const std::string& /*error*/),
              (override));
  MOCK_METHOD(void, OnTracePackets, (std::vector<TracePacket>*, bool));
  MOCK_METHOD(void, OnDetach, (bool), (override));
  MOCK_METHOD(void, OnAttach, (bool, const TraceConfig&), (override));
  MOCK_METHOD(void, OnTraceStats, (bool, const TraceStats&), (override));
  MOCK_METHOD(void, OnObservableEvents, (const ObservableEvents&), (override));
  MOCK_METHOD(void, OnSessionCloned, (const OnSessionClonedArgs&), (override));

  // Workaround, gmock doesn't support yet move-only types, passing a pointer.
  void OnTraceData(std::vector<TracePacket> packets, bool has_more) {
    OnTracePackets(&packets, has_more);
  }
};

void CheckTraceStats(const protos::gen::TracePacket& packet) {
  EXPECT_TRUE(packet.has_trace_stats());
  EXPECT_GE(packet.trace_stats().producers_seen(), 1u);
  EXPECT_EQ(1u, packet.trace_stats().producers_connected());
  EXPECT_EQ(1u, packet.trace_stats().data_sources_registered());
  EXPECT_EQ(1u, packet.trace_stats().tracing_sessions());
  EXPECT_EQ(1u, packet.trace_stats().total_buffers());
  EXPECT_EQ(1, packet.trace_stats().buffer_stats_size());

  const auto& buf_stats = packet.trace_stats().buffer_stats()[0];
  EXPECT_GT(buf_stats.bytes_written(), 0u);
  EXPECT_GT(buf_stats.chunks_written(), 0u);
  EXPECT_EQ(0u, buf_stats.chunks_overwritten());
  EXPECT_EQ(0u, buf_stats.chunks_rewritten());
  EXPECT_EQ(0u, buf_stats.chunks_committed_out_of_order());
  EXPECT_EQ(0u, buf_stats.write_wrap_count());
  EXPECT_EQ(0u, buf_stats.patches_failed());
  EXPECT_EQ(0u, buf_stats.readaheads_failed());
  EXPECT_EQ(0u, buf_stats.abi_violations());
}

static_assert(TracingServiceImpl::kMaxTracePacketSliceSize <=
                  ipc::kIPCBufferSize - 512,
              "Tracing service max packet slice should be smaller than IPC "
              "buffer size (with some headroom)");

}  // namespace

class TracingIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
    kProducerSock.Destroy();
    kConsumerSock.Destroy();
    task_runner_.reset(new base::TestTaskRunner());

    // Create the service host.
    svc_ = ServiceIPCHost::CreateInstance(task_runner_.get());
    svc_->Start(kProducerSock.name(), kConsumerSock.name());

    // Create and connect a Producer.
    producer_endpoint_ = ProducerIPCClient::Connect(
        kProducerSock.name(), &producer_, "perfetto.mock_producer",
        task_runner_.get(), GetProducerSMBScrapingMode());
    auto on_producer_connect =
        task_runner_->CreateCheckpoint("on_producer_connect");
    EXPECT_CALL(producer_, OnConnect()).WillOnce(on_producer_connect);
    task_runner_->RunUntilCheckpoint("on_producer_connect");

    // Register a data source.
    DataSourceDescriptor ds_desc;
    ds_desc.set_name("perfetto.test");
    producer_endpoint_->RegisterDataSource(ds_desc);

    // Create and connect a Consumer.
    consumer_endpoint_ = ConsumerIPCClient::Connect(
        kConsumerSock.name(), &consumer_, task_runner_.get());
    auto on_consumer_connect =
        task_runner_->CreateCheckpoint("on_consumer_connect");
    EXPECT_CALL(consumer_, OnConnect()).WillOnce(on_consumer_connect);
    task_runner_->RunUntilCheckpoint("on_consumer_connect");

    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&producer_));
    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&consumer_));
  }

  void TearDown() override {
    // Destroy the service and check that both Producer and Consumer see an
    // OnDisconnect() call.

    auto on_producer_disconnect =
        task_runner_->CreateCheckpoint("on_producer_disconnect");
    if (producer_endpoint_)
      EXPECT_CALL(producer_, OnDisconnect()).WillOnce(on_producer_disconnect);

    auto on_consumer_disconnect =
        task_runner_->CreateCheckpoint("on_consumer_disconnect");
    EXPECT_CALL(consumer_, OnDisconnect()).WillOnce(on_consumer_disconnect);

    svc_.reset();
    if (producer_endpoint_)
      task_runner_->RunUntilCheckpoint("on_producer_disconnect");
    task_runner_->RunUntilCheckpoint("on_consumer_disconnect");

    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&producer_));
    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&consumer_));

    task_runner_.reset();
    kProducerSock.Destroy();
    kConsumerSock.Destroy();
  }

  virtual TracingService::ProducerSMBScrapingMode GetProducerSMBScrapingMode() {
    return TracingService::ProducerSMBScrapingMode::kDefault;
  }

  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<ServiceIPCHost> svc_;
  std::unique_ptr<TracingService::ProducerEndpoint> producer_endpoint_;
  MockProducer producer_;
  std::unique_ptr<TracingService::ConsumerEndpoint> consumer_endpoint_;
  MockConsumer consumer_;
};

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
class RingBufferTransportIntegrationTest : public TracingIntegrationTest {
 protected:
  std::vector<DataSourceConfig> Start(TraceConfig config, size_t instances) {
    std::vector<DataSourceConfig> setups;
    auto started = task_runner_->CreateCheckpoint("ring_buffer_started");
    size_t starts = 0;
    EXPECT_CALL(producer_, OnTracingSetup());
    EXPECT_CALL(producer_, SetupDataSource(_, _))
        .Times(static_cast<int>(instances))
        .WillRepeatedly([&](DataSourceInstanceID, const DataSourceConfig& cfg) {
          setups.push_back(cfg);
        });
    EXPECT_CALL(producer_, StartDataSource(_, _))
        .Times(static_cast<int>(instances))
        .WillRepeatedly([&](DataSourceInstanceID, const DataSourceConfig&) {
          if (++starts == instances)
            started();
        });
    consumer_endpoint_->EnableTracing(config);
    task_runner_->RunUntilCheckpoint("ring_buffer_started");
    return setups;
  }

  void Offer() {
    memory_ = PosixSharedMemory::Create(sizeof(tracing_v2::RingBufferHeader) +
                                        16 * 256);
    ring_buffer_ = std::make_unique<tracing_v2::SharedRingBuffer>(
        static_cast<uint8_t*>(memory_->start()), memory_->size(), 256);
    auto offered = task_runner_->CreateCheckpoint("ring_buffer_offered");
    test::ProducerIPCClientOfferForTest::OfferRingBuffer(
        client(), memory_->fd(), 256, [offered](bool success) {
          EXPECT_TRUE(success);
          offered();
        });
    task_runner_->RunUntilCheckpoint("ring_buffer_offered");
  }

  ProducerIPCClientImpl* client() {
    return static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get());
  }

  void Drain() {
    std::string name = "drain_" + std::to_string(next_checkpoint_++);
    auto drained = task_runner_->CreateCheckpoint(name);
    producer_endpoint_->DrainRingBuffer();
    producer_endpoint_->Sync(drained);
    task_runner_->RunUntilCheckpoint(name);
  }

  std::vector<protos::gen::TracePacket> Read() {
    std::vector<protos::gen::TracePacket> result;
    auto read = task_runner_->CreateCheckpoint("ring_buffer_read");
    EXPECT_CALL(consumer_, OnTracePackets(_, _))
        .WillRepeatedly([&](std::vector<TracePacket>* packets, bool more) {
          for (const auto& encoded : *packets) {
            protos::gen::TracePacket packet;
            EXPECT_TRUE(
                packet.ParseFromString(encoded.GetRawBytesForTesting()));
            if (packet.has_for_testing())
              result.push_back(std::move(packet));
          }
          if (!more)
            read();
        });
    consumer_endpoint_->ReadBuffers();
    task_runner_->RunUntilCheckpoint("ring_buffer_read");
    testing::Mock::VerifyAndClearExpectations(&consumer_);
    return result;
  }

  static std::string Packet(const std::string& value) {
    return std::string("\xa3\x38\x0a") + static_cast<char>(value.size()) +
           value + '\x04';
  }
  std::unique_ptr<PosixSharedMemory> memory_;
  std::unique_ptr<tracing_v2::SharedRingBuffer> ring_buffer_;
  size_t next_checkpoint_ = 0;
};

TEST_F(RingBufferTransportIntegrationTest,
       InstanceWriterReachesConsumerWithoutSdk) {
  DataSourceDescriptor descriptor;
  descriptor.set_name("perfetto.ring_endpoint");
  producer_endpoint_->RegisterDataSource(descriptor);

  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* source = config.add_data_sources()->mutable_config();
  source->set_name(descriptor.name());
  source->mutable_experimental_tracing_v2()->set_use_v2_probability_percent(
      100);
  source->mutable_experimental_tracing_v2()->set_chunk_size_bytes(256);
  const std::string payload(2000, 'r');
  std::unique_ptr<TraceWriter> writer;
  DataSourceInstanceID instance = 0;
  BufferID target_buffer = 0;
  auto started = task_runner_->CreateCheckpoint("instance_started");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _))
      .WillOnce([&](DataSourceInstanceID id, const DataSourceConfig& setup) {
        instance = id;
        target_buffer = static_cast<BufferID>(setup.target_buffer());
        EXPECT_TRUE(setup.tracing_v2_eligible());
        EXPECT_NE(setup.target_buffer(), 0u);
        writer = producer_endpoint_->CreateTraceWriter(
            target_buffer, BufferExhaustedPolicy::kDrop, instance);
        auto packet = writer->NewTracePacket();
        EXPECT_EQ(packet->encoding(),
                  protozero::Message::NestedEncoding::kProtoGroup);
        packet->set_for_testing()->set_str(payload);
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([&](DataSourceInstanceID id, const DataSourceConfig&) {
        EXPECT_EQ(id, instance);
        started();
      });
  consumer_endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("instance_started");

  auto written = task_runner_->CreateCheckpoint("instance_written");
  writer->Flush(written);
  task_runner_->RunUntilCheckpoint("instance_written");

  EXPECT_CALL(producer_, Flush(_, _, _, _))
      .WillOnce([&](FlushRequestID id, const DataSourceInstanceID* instances,
                    size_t count, FlushFlags) {
        ASSERT_EQ(count, 1u);
        EXPECT_EQ(instances[0], instance);
        writer->Flush();
        producer_endpoint_->NotifyFlushComplete(id);
      });
  auto flushed = task_runner_->CreateCheckpoint("instance_flushed");
  consumer_endpoint_->Flush(10000, [flushed](bool success) {
    EXPECT_TRUE(success);
    flushed();
  });
  task_runner_->RunUntilCheckpoint("instance_flushed");
  auto packets = Read();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), payload);

  writer.reset();
  auto stopped = task_runner_->CreateCheckpoint("instance_stopped");
  EXPECT_CALL(producer_, StopDataSource(instance));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(stopped));
  consumer_endpoint_->DisableTracing();
  task_runner_->RunUntilCheckpoint("instance_stopped");
}

TEST_F(RingBufferTransportIntegrationTest, RejectionStopsV2WithoutFallback) {
  // Reserve the service's one ring so the automatic offer receives a rejection.
  Offer();
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* source = config.add_data_sources()->mutable_config();
  source->set_name("perfetto.test");
  source->mutable_experimental_tracing_v2()->set_use_v2_probability_percent(
      100);

  DataSourceInstanceID instance = 0;
  BufferID target = 0;
  std::unique_ptr<TraceWriter> early_writer;
  auto started = task_runner_->CreateCheckpoint("rejected_instance_started");
  auto early_flushed = task_runner_->CreateCheckpoint("rejected_early_flushed");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _))
      .WillOnce([&](DataSourceInstanceID id, const DataSourceConfig& setup) {
        instance = id;
        target = static_cast<BufferID>(setup.target_buffer());
        early_writer = producer_endpoint_->CreateTraceWriter(
            target, BufferExhaustedPolicy::kDrop, instance);
        EXPECT_NE(early_writer->writer_id(), 0u);
        early_writer->NewTracePacket()->set_for_testing()->set_str("early");
        early_writer->Flush(early_flushed);
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce(InvokeWithoutArgs(started));
  consumer_endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("rejected_instance_started");
  task_runner_->RunUntilCheckpoint("rejected_early_flushed");

  // This writer still borrows the rejected mapping. Further packets are
  // dropped.
  early_writer->NewTracePacket()->set_for_testing()->set_str("after rejection");
  EXPECT_GT(early_writer->drop_count(), 0u);
  auto writer = producer_endpoint_->CreateTraceWriter(
      target, BufferExhaustedPolicy::kDrop, instance);
  EXPECT_EQ(writer->writer_id(), 0u);
  writer->NewTracePacket()->set_for_testing()->set_str("no fallback");

  // The connection and its independent v1 writer path remain available.
  auto legacy = producer_endpoint_->CreateTraceWriter(
      target, BufferExhaustedPolicy::kDrop);
  legacy->NewTracePacket()->set_for_testing()->set_str("legacy");
  auto committed = task_runner_->CreateCheckpoint("legacy_after_rejection");
  legacy->Flush(committed);
  task_runner_->RunUntilCheckpoint("legacy_after_rejection");
  auto packets = Read();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "legacy");
  EXPECT_TRUE(producer_endpoint_->SupportsTracingV2());
}

TEST_F(RingBufferTransportIntegrationTest, LossBeforeFirstRouteCountsDiscard) {
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  auto setups = Start(config, 1);
  ASSERT_EQ(setups.size(), 1u);
  Offer();
  auto writer = tracing_v2::test::MakeWriter(
      ring_buffer_.get(), 1, static_cast<BufferID>(setups[0].target_buffer()));
  // Publish only loss, before any fragment establishes a destination.
  writer.BeginFragment(1, false);
  writer.RecordDataLoss();
  writer.FinishCurrentChunk();
  Drain();
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("first")));
  writer.FinishCurrentChunk();
  Drain();
  auto packets = Read();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "first");
  EXPECT_FALSE(packets[0].previous_packet_dropped());
  auto stats_read = task_runner_->CreateCheckpoint("loss_stats_read");
  EXPECT_CALL(consumer_, OnTraceStats(true, _))
      .WillOnce([&](bool, const TraceStats& stats) {
        EXPECT_EQ(stats.chunks_discarded(), 1u);
        stats_read();
      });
  consumer_endpoint_->GetTraceStats();
  task_runner_->RunUntilCheckpoint("loss_stats_read");
}

TEST_F(RingBufferTransportIntegrationTest, WriterLossStaysAtItsDestination) {
  TraceConfig config;
  for (uint32_t i = 0; i < 2; ++i) {
    auto* buffer = config.add_buffers();
    buffer->set_size_kb(64);
    buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
    auto* source = config.add_data_sources()->mutable_config();
    source->set_name("perfetto.test");
    source->set_target_buffer(i);
  }
  auto setups = Start(config, 2);
  ASSERT_EQ(setups.size(), 2u);
  Offer();
  auto first = tracing_v2::test::MakeWriter(
      ring_buffer_.get(), 1, static_cast<BufferID>(setups[0].target_buffer()));
  auto second = tracing_v2::test::MakeWriter(
      ring_buffer_.get(), 2, static_cast<BufferID>(setups[1].target_buffer()));
  ASSERT_TRUE(tracing_v2::test::WriteFragment(
      &first, Packet("partial").substr(0, 3), false, true));
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&second, Packet("other-before")));
  first.FinishCurrentChunk();
  second.FinishCurrentChunk();
  Drain();
  first.RecordDataLoss();
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&first, Packet("discarded")));
  first.FinishCurrentChunk();
  Drain();
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&first, Packet("recovered")));
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&second, Packet("other-after")));
  first.FinishCurrentChunk();
  second.FinishCurrentChunk();
  Drain();
  auto packets = Read();
  ASSERT_EQ(packets.size(), 3u);
  for (const auto& packet : packets) {
    if (packet.for_testing().str() == "recovered")
      EXPECT_TRUE(packet.previous_packet_dropped());
    else if (packet.for_testing().str() == "other-after")
      EXPECT_FALSE(packet.previous_packet_dropped());
    else
      EXPECT_EQ(packet.for_testing().str(), "other-before");
  }
}

TEST_F(RingBufferTransportIntegrationTest,
       RejectsIncompatibleAndForbiddenDestinations) {
  TraceConfig config;
  for (uint32_t i = 0; i < 2; ++i) {
    auto* buffer = config.add_buffers();
    buffer->set_size_kb(64);
    if (i == 1)
      buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
    auto* source = config.add_data_sources()->mutable_config();
    source->set_name("perfetto.test");
    source->set_target_buffer(i);
  }
  // This unregistered source can install a ProtoVM later. It must already
  // exclude the second buffer when the first two instances receive setup.
  auto* late = config.add_data_sources()->mutable_config();
  late->set_name("late-vm");
  late->set_target_buffer(1);
  late->mutable_protovm_config()->set_memory_limit_kb(64);
  auto setups = Start(config, 2);
  ASSERT_EQ(setups.size(), 2u);
  EXPECT_FALSE(setups[0].tracing_v2_eligible());
  EXPECT_FALSE(setups[1].tracing_v2_eligible());
  Offer();
  for (uint32_t i = 0; i < 3; ++i) {
    BufferID target =
        i < 2 ? static_cast<BufferID>(setups[i].target_buffer()) : 0;
    auto writer = tracing_v2::test::MakeWriter(
        ring_buffer_.get(), static_cast<WriterID>(i + 1), target);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("rejected")));
  }
  Drain();
  EXPECT_TRUE(Read().empty());
}

TEST_F(RingBufferTransportIntegrationTest,
       DisconnectRecoversUnnotifiedPublication) {
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  auto setups = Start(config, 1);
  ASSERT_EQ(setups.size(), 1u);
  Offer();
  {
    auto writer = tracing_v2::test::MakeWriter(
        ring_buffer_.get(), 1,
        static_cast<BufferID>(setups[0].target_buffer()));
    ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("unnotified")));
  }
  EXPECT_CALL(producer_, OnDisconnect()).Times(1);
  producer_endpoint_->Disconnect();
  producer_endpoint_.reset();
  task_runner_->RunUntilIdle();
  auto packets = Read();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "unnotified");
}

TEST_F(RingBufferTransportIntegrationTest, CorruptRingBufferKeepsConnection) {
  Offer();
  {
    auto writer = tracing_v2::test::MakeWriter(ring_buffer_.get(), 1, 1);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("corrupt")));
  }
  tracing_v2::test::SharedRingBufferInternalsForTest::SetChunkStateWord(
      ring_buffer_.get(), tracing_v2::ChunkIndex::FromIndex(0),
      static_cast<uint32_t>(tracing_v2::ChunkState::kReserved5));
  Drain();
  Drain();
  EXPECT_TRUE(producer_endpoint_->SupportsTracingV2());
}

TEST_F(RingBufferTransportIntegrationTest, EarlyPublicationAndDrain) {
  ASSERT_TRUE(producer_endpoint_->SupportsTracingV2());
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  BufferID target = 0;
  auto started = task_runner_->CreateCheckpoint("ring_buffer_started");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _))
      .WillOnce([&](DataSourceInstanceID, const DataSourceConfig& cfg) {
        EXPECT_TRUE(cfg.tracing_v2_eligible());
        target = static_cast<BufferID>(cfg.target_buffer());
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce(InvokeWithoutArgs(started));
  consumer_endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("ring_buffer_started");

  auto memory =
      PosixSharedMemory::Create(sizeof(tracing_v2::RingBufferHeader) + 4 * 256);
  tracing_v2::SharedRingBuffer ring_buffer(
      static_cast<uint8_t*>(memory->start()), memory->size(), 256);
  auto writer = tracing_v2::test::MakeWriter(&ring_buffer, 2, target);
  // Field 900 contains TestEvent.str, with the internal bare closing marker.
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer,
                                              "\xa3\x38\x0a\x05"
                                              "early\x04"));
  writer.FinishCurrentChunk();
  auto offered = task_runner_->CreateCheckpoint("ring_buffer_offered");
  test::ProducerIPCClientOfferForTest::OfferRingBuffer(
      client(), memory->fd(), 256, [offered](bool success) {
        EXPECT_TRUE(success);
        offered();
      });
  task_runner_->RunUntilCheckpoint("ring_buffer_offered");
  EXPECT_EQ(tracing_v2::test::SharedRingBufferInternalsForTest::GetReadPos(
                &ring_buffer),
            1u);

  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer,
                                              "\xa3\x38\x0a\x05"
                                              "later\x04"));
  writer.FinishCurrentChunk();
  auto drained = task_runner_->CreateCheckpoint("ring_buffer_drained");
  producer_endpoint_->DrainRingBuffer();
  producer_endpoint_->Sync([&] {
    EXPECT_EQ(tracing_v2::test::SharedRingBufferInternalsForTest::GetReadPos(
                  &ring_buffer),
              2u);
    drained();
  });
  task_runner_->RunUntilCheckpoint("ring_buffer_drained");

  // The manual ring writer uses ID 2. The first v1 writer receives ID 1.
  auto legacy = producer_endpoint_->CreateTraceWriter(
      target, BufferExhaustedPolicy::kDrop);
  legacy->NewTracePacket()->set_for_testing()->set_str("legacy");
  auto committed = task_runner_->CreateCheckpoint("legacy_committed");
  legacy->Flush(committed);
  task_runner_->RunUntilCheckpoint("legacy_committed");
  std::vector<std::string> values;
  auto read = task_runner_->CreateCheckpoint("ring_buffer_read");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&](std::vector<TracePacket>* packets, bool more) {
        for (const auto& encoded : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(packet.ParseFromString(encoded.GetRawBytesForTesting()));
          if (packet.has_for_testing())
            values.push_back(packet.for_testing().str());
          if (packet.has_trace_config())
            EXPECT_FALSE(packet.trace_config()
                             .data_sources()[0]
                             .config()
                             .has_tracing_v2_eligible());
        }
        if (!more)
          read();
      });
  consumer_endpoint_->ReadBuffers();
  task_runner_->RunUntilCheckpoint("ring_buffer_read");
  EXPECT_THAT(values,
              testing::UnorderedElementsAre("early", "later", "legacy"));
}

TEST_F(RingBufferTransportIntegrationTest, RejectsInvalidAndDuplicateOffers) {
  auto offer = [&](size_t size, uint32_t chunk_size, bool accepted,
                   const char* checkpoint) {
    auto memory = PosixSharedMemory::Create(size);
    auto done = task_runner_->CreateCheckpoint(checkpoint);
    test::ProducerIPCClientOfferForTest::OfferRingBuffer(
        client(), memory->fd(), chunk_size, [=](bool result) {
          EXPECT_EQ(result, accepted);
          done();
        });
    task_runner_->RunUntilCheckpoint(checkpoint);
  };
  // An invalid layout does not persist state on the peer. A second offer with
  // a valid layout can still succeed.
  offer(4096, 256, false, "invalid_layout");
  offer(sizeof(tracing_v2::RingBufferHeader) + 1024, 256, true, "first_valid");
  // The service accepts one layout per producer. A later offer is rejected.
  offer(sizeof(tracing_v2::RingBufferHeader) + 1024, 256, false,
        "duplicate_offer");
  auto synced = task_runner_->CreateCheckpoint("still_connected");
  producer_endpoint_->Sync(synced);
  task_runner_->RunUntilCheckpoint("still_connected");
  EXPECT_TRUE(producer_endpoint_->SupportsTracingV2());
}
#endif

TEST_F(TracingIntegrationTest, WithIPCTransport) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  consumer_endpoint_->EnableTracing(trace_config);

  // At this point, the Producer should be asked to turn its data source on.
  DataSourceInstanceID ds_iid = 0;

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());

  // Store the arguments passed to SetupDataSource() and later check that they
  // match the ones passed to StartDataSource().
  DataSourceInstanceID setup_id;
  DataSourceConfig setup_cfg_proto;
  EXPECT_CALL(producer_, SetupDataSource(_, _))
      .WillOnce([&setup_id, &setup_cfg_proto](DataSourceInstanceID id,
                                              const DataSourceConfig& cfg) {
        setup_id = id;
        setup_cfg_proto = cfg;
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &ds_iid, &global_buf_id, &setup_id,
                 &setup_cfg_proto](DataSourceInstanceID id,
                                   const DataSourceConfig& cfg) {
        // id and config should match the ones passed to SetupDataSource.
        ASSERT_EQ(id, setup_id);
        ASSERT_EQ(setup_cfg_proto, cfg);
        ASSERT_NE(0u, id);
        ds_iid = id;
        ASSERT_EQ("perfetto.test", cfg.name());
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        ASSERT_NE(0u, global_buf_id);
        ASSERT_LE(global_buf_id, std::numeric_limits<BufferID>::max());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  // Now let the data source fill some pages within the same task.
  // Doing so should accumulate a bunch of chunks that will be notified by the
  // a future task in one batch.
  std::unique_ptr<TraceWriter> writer = producer_endpoint_->CreateTraceWriter(
      global_buf_id, BufferExhaustedPolicy::kStall);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[16];
    base::SprintfTrunc(buf, sizeof(buf), "evt_%zu", i);
    writer->NewTracePacket()->set_for_testing()->set_str(buf, strlen(buf));
  }

  // Allow the service to see the CommitData() before reading back.
  auto on_data_committed = task_runner_->CreateCheckpoint("on_data_committed");
  writer->Flush(on_data_committed);
  task_runner_->RunUntilCheckpoint("on_data_committed");

  // Read the log buffer.
  consumer_endpoint_->ReadBuffers();
  size_t num_pack_rx = 0;
  bool saw_clock_snapshot = false;
  bool saw_trace_config = false;
  bool saw_trace_stats = false;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&num_pack_rx, all_packets_rx, &trace_config,
                       &saw_clock_snapshot, &saw_trace_config,
                       &saw_trace_stats](std::vector<TracePacket>* packets,
                                         bool has_more) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
        const int kExpectedMinNumberOfClocks = 1;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
        const int kExpectedMinNumberOfClocks = 2;
#else
        const int kExpectedMinNumberOfClocks = 6;
#endif

        for (auto& encoded_packet : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(
              packet.ParseFromString(encoded_packet.GetRawBytesForTesting()));
          if (packet.has_for_testing()) {
            char buf[8];
            base::SprintfTrunc(buf, sizeof(buf), "evt_%zu", num_pack_rx++);
            EXPECT_EQ(std::string(buf), packet.for_testing().str());
          } else if (packet.has_clock_snapshot()) {
            EXPECT_GE(packet.clock_snapshot().clocks_size(),
                      kExpectedMinNumberOfClocks);
            saw_clock_snapshot = true;
          } else if (packet.has_trace_config()) {
            EXPECT_EQ(packet.trace_config(), trace_config);
            saw_trace_config = true;
          } else if (packet.has_trace_stats()) {
            saw_trace_stats = true;
            CheckTraceStats(packet);
          }
        }
        if (!has_more)
          all_packets_rx();
      });
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  ASSERT_EQ(kNumPackets, num_pack_rx);
  EXPECT_TRUE(saw_clock_snapshot);
  EXPECT_TRUE(saw_trace_config);
  EXPECT_TRUE(saw_trace_stats);

  // Disable tracing.
  consumer_endpoint_->DisableTracing();

  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(producer_, StopDataSource(_));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
}

// Regression test for b/172950370.
TEST_F(TracingIntegrationTest, ValidErrorOnDisconnection) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  consumer_endpoint_->EnableTracing(trace_config);

  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());

  // Store the arguments passed to SetupDataSource() and later check that they
  // match the ones passed to StartDataSource().
  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce(InvokeWithoutArgs(on_create_ds_instance));
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce([](const std::string& err) {
        EXPECT_THAT(err,
                    testing::HasSubstr("EnableTracing IPC request rejected"));
      });

  // TearDown() will destroy the service via svc_.reset(). That will drop the
  // connection and trigger the EXPECT_CALL(OnTracingDisabled) above.
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST_F(TracingIntegrationTest, WriteIntoFile) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  trace_config.set_write_into_file(true);
  base::TempFile tmp_file = base::TempFile::CreateUnlinked();
  consumer_endpoint_->EnableTracing(trace_config,
                                    base::ScopedFile(dup(tmp_file.fd())));

  // At this point, the producer_ should be asked to turn its data source on.
  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &global_buf_id](
                    DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  std::unique_ptr<TraceWriter> writer = producer_endpoint_->CreateTraceWriter(
      global_buf_id, BufferExhaustedPolicy::kStall);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[16];
    base::SprintfTrunc(buf, sizeof(buf), "evt_%zu", i);
    writer->NewTracePacket()->set_for_testing()->set_str(buf, strlen(buf));
  }
  auto on_data_committed = task_runner_->CreateCheckpoint("on_data_committed");
  writer->Flush(on_data_committed);
  task_runner_->RunUntilCheckpoint("on_data_committed");

  // Will disable tracing and will force the buffers to be written into the
  // file before destroying them.
  consumer_endpoint_->FreeBuffers();

  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(producer_, StopDataSource(_));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");

  // Check that |tmp_file| contains a valid trace.proto message.
  ASSERT_EQ(0, lseek(tmp_file.fd(), 0, SEEK_SET));
  std::string trace_contents;
  ASSERT_TRUE(base::ReadFileDescriptor(tmp_file.fd(), &trace_contents));
  protos::gen::Trace tmp_trace;
  ASSERT_TRUE(tmp_trace.ParseFromString(trace_contents));
  size_t num_test_packet = 0;
  size_t num_clock_snapshot_packet = 0;
  size_t num_system_info_packet = 0;
  bool saw_trace_stats = false;
  for (int i = 0; i < tmp_trace.packet_size(); i++) {
    const auto& packet = tmp_trace.packet()[static_cast<size_t>(i)];
    if (packet.has_for_testing()) {
      ASSERT_EQ("evt_" + std::to_string(num_test_packet++),
                packet.for_testing().str());
    } else if (packet.has_trace_stats()) {
      saw_trace_stats = true;
      CheckTraceStats(packet);
    } else if (packet.has_clock_snapshot()) {
      num_clock_snapshot_packet++;
    } else if (packet.has_system_info()) {
      num_system_info_packet++;
    }
  }
  ASSERT_TRUE(saw_trace_stats);
  ASSERT_GT(num_clock_snapshot_packet, 0u);
  ASSERT_GT(num_system_info_packet, 0u);
}
#endif

class TracingIntegrationTestWithSMBScrapingProducer
    : public TracingIntegrationTest {
 public:
  TracingService::ProducerSMBScrapingMode GetProducerSMBScrapingMode()
      override {
    return TracingService::ProducerSMBScrapingMode::kEnabled;
  }
};

TEST_F(TracingIntegrationTestWithSMBScrapingProducer, ScrapeOnFlush) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  consumer_endpoint_->EnableTracing(trace_config);

  // At this point, the Producer should be asked to turn its data source on.

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());

  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &global_buf_id](
                    DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  // Create writer, which will post a task to register the writer with the
  // service.
  std::unique_ptr<TraceWriter> writer = producer_endpoint_->CreateTraceWriter(
      global_buf_id, BufferExhaustedPolicy::kStall);
  ASSERT_TRUE(writer);

  // Wait for the writer to be registered.
  task_runner_->RunUntilIdle();

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  // Ask the service to flush, but don't flush our trace writer. This should
  // cause our uncommitted SMB chunk to be scraped.
  auto on_flush_complete = task_runner_->CreateCheckpoint("on_flush_complete");
  FlushFlags flush_flags(FlushFlags::Initiator::kConsumerSdk,
                         FlushFlags::Reason::kExplicit);
  consumer_endpoint_->Flush(
      5000,
      [on_flush_complete](bool success) {
        EXPECT_TRUE(success);
        on_flush_complete();
      },
      flush_flags);
  EXPECT_CALL(producer_, Flush(_, _, _, flush_flags))
      .WillOnce([this](FlushRequestID flush_req_id, const DataSourceInstanceID*,
                       size_t, FlushFlags) {
        producer_endpoint_->NotifyFlushComplete(flush_req_id);
      });
  task_runner_->RunUntilCheckpoint("on_flush_complete");

  // Read the log buffer. We should see all the packets.
  consumer_endpoint_->ReadBuffers();

  size_t num_test_pack_rx = 0;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&num_test_pack_rx, all_packets_rx](
                          std::vector<TracePacket>* packets, bool has_more) {
        for (auto& encoded_packet : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(
              packet.ParseFromString(encoded_packet.GetRawBytesForTesting()));
          if (packet.has_for_testing()) {
            num_test_pack_rx++;
          }
        }
        if (!has_more)
          all_packets_rx();
      });
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  ASSERT_EQ(3u, num_test_pack_rx);

  // Disable tracing.
  consumer_endpoint_->DisableTracing();

  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  auto on_stop_ds = task_runner_->CreateCheckpoint("on_stop_ds");
  EXPECT_CALL(producer_, StopDataSource(_))
      .WillOnce(InvokeWithoutArgs(on_stop_ds));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_stop_ds");
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
}

// TODO(primiano): add tests to cover:
// - unknown fields preserved end-to-end.
// - >1 data source.
// - >1 data consumer sharing the same data source, with different TraceBuffers.
// - >1 consumer with > 1 buffer each.
// - Consumer disconnecting in the middle of a ReadBuffers() call.
// - Multiple calls to DisableTracing.
// - Out of order Enable/Disable/FreeBuffers calls.
// - DisableTracing does actually freeze the buffers.

}  // namespace perfetto
