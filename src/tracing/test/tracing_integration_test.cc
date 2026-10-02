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
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/ipc/client.h"
#include "perfetto/ext/tracing/core/consumer.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/ipc/consumer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "perfetto/protozero/message.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"
#include "src/tracing/ipc/producer/producer_ipc_client_impl.h"
#include "src/tracing/service/tracing_service_impl.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include "src/tracing/ipc/posix_shared_memory.h"
#endif

#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/ipc/producer_port.gen.h"
#include "protos/perfetto/ipc/producer_port.ipc.h"
#include "protos/perfetto/trace/clock_snapshot.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace test {

class ProducerIPCClientTestPeer {
 public:
  static void ScheduleDisconnect(ProducerIPCClientImpl* client) {
    client->ScheduleDisconnect();
  }

  static void OnServiceRequest(
      ProducerIPCClientImpl* client,
      const protos::gen::GetAsyncCommandResponse& cmd) {
    client->OnServiceRequest(cmd);
  }

  static const base::FlatSet<ProtocolAbiVersion>& protocol_abi_versions(
      const ProducerIPCClientImpl* client) {
    return client->protocol_abi_versions_;
  }

  // Acts like an InitializeConnection reply to |offered_versions|.
  static void OnConnectionInitialized(
      ProducerIPCClientImpl* client,
      const std::vector<ProtocolAbiVersion>& offered_versions,
      const std::vector<ProtocolAbiVersion>& protocol_abi_versions,
      bool use_shmem_emulation = false,
      bool connection_succeeded = true) {
    ipc::AsyncResult<protos::gen::InitializeConnectionResponse> response;
    if (connection_succeeded) {
      response =
          ipc::AsyncResult<protos::gen::InitializeConnectionResponse>::Create();
      response->set_direct_smb_patching_supported(true);
      response->set_use_shmem_emulation(use_shmem_emulation);
      for (auto version : protocol_abi_versions) {
        response->add_protocol_abi_versions(
            static_cast<protos::gen::ProtocolAbiVersion>(version));
      }
    }
    client->OnConnectionInitialized(offered_versions, std::move(response));
  }
};

}  // namespace test

namespace {

using testing::_;
using testing::ElementsAre;
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

// A protocol error in an IPC handler drops the connection in two steps. The
// producer must get OnDisconnect(), and calls between the two steps must not
// use the dropped port.
TEST_F(TracingIntegrationTest, ScheduledDisconnectCompletes) {
  auto* client = static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get());
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);

  test::ProducerIPCClientTestPeer::ScheduleDisconnect(client);
  DataSourceDescriptor descriptor;
  descriptor.set_name("perfetto.test");
  producer_endpoint_->RegisterDataSource(descriptor);
  producer_endpoint_->UpdateDataSource(descriptor);
  producer_endpoint_->NotifyDataSourceStarted(1);

  task_runner_->RunUntilCheckpoint("producer_disconnected");
  task_runner_->RunUntilIdle();
  EXPECT_EQ(client->GetClientForTesting(), nullptr);
  producer_endpoint_.reset();
}

TEST_F(TracingIntegrationTest, DisconnectCompletesScheduledDisconnect) {
  auto* client = static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get());
  bool disconnected = false;
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce([&] { disconnected = true; });

  test::ProducerIPCClientTestPeer::ScheduleDisconnect(client);
  EXPECT_FALSE(disconnected);
  producer_endpoint_->Disconnect();
  EXPECT_TRUE(disconnected);
  EXPECT_EQ(client->GetClientForTesting(), nullptr);

  producer_endpoint_->Disconnect();
  task_runner_->RunUntilIdle();
  producer_endpoint_.reset();
}

TEST_F(TracingIntegrationTest, ScheduledDisconnectRejectsPendingSync) {
  auto* client = static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get());
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);

  bool sync_called = false;
  producer_endpoint_->Sync([&] {
    sync_called = true;
    producer_endpoint_->NotifyDataSourceStarted(1);
  });
  EXPECT_FALSE(sync_called);

  // Destroying the proxy rejects Sync() before the task runner resumes.
  test::ProducerIPCClientTestPeer::ScheduleDisconnect(client);
  EXPECT_TRUE(sync_called);
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  producer_endpoint_.reset();
}

TEST_F(TracingIntegrationTest, DisconnectRejectsPendingSync) {
  EXPECT_CALL(producer_, OnDisconnect()).Times(1);
  bool sync_called = false;
  producer_endpoint_->Sync([&] {
    sync_called = true;
    producer_endpoint_->NotifyDataSourceStarted(1);
  });
  EXPECT_FALSE(sync_called);

  producer_endpoint_->Disconnect();
  EXPECT_TRUE(sync_called);
  task_runner_->RunUntilIdle();
  producer_endpoint_.reset();
}

TEST_F(TracingIntegrationTest, DestroyEndpointWithScheduledDisconnect) {
  EXPECT_CALL(producer_, OnDisconnect()).Times(0);
  test::ProducerIPCClientTestPeer::ScheduleDisconnect(
      static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get()));
  producer_endpoint_.reset();
  task_runner_->RunUntilIdle();
}

TEST_F(TracingIntegrationTest, SetupTracingWithoutSmbDisconnects) {
  auto* client = static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get());
  ASSERT_EQ(client->shared_memory(), nullptr);
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnTracingSetup()).Times(0);
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce([&] {
    EXPECT_EQ(client->GetClientForTesting(), nullptr);
    producer_endpoint_.reset();
    disconnected();
  });

  protos::gen::GetAsyncCommandResponse cmd;
  cmd.mutable_setup_tracing()->set_shared_buffer_page_size_kb(4);
  test::ProducerIPCClientTestPeer::OnServiceRequest(client, cmd);

  // The handler must return before the disconnect destroys the endpoint.
  ASSERT_TRUE(producer_endpoint_);
  EXPECT_NE(client->GetClientForTesting(), nullptr);
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  task_runner_->RunUntilIdle();
  EXPECT_FALSE(producer_endpoint_);
}

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

  void Attach() {
    memory_ = PosixSharedMemory::Create(sizeof(tracing_v2::RingBufferHeader) +
                                        16 * 256);
    ring_buffer_ = std::make_unique<tracing_v2::SharedRingBuffer>(
        static_cast<uint8_t*>(memory_->start()), memory_->size(), 256);
    auto attached = task_runner_->CreateCheckpoint("ring_buffer_attached");
    producer_endpoint_->AttachV2RingBuffer(memory_, 256,
                                           [attached](bool success) {
                                             EXPECT_TRUE(success);
                                             attached();
                                           });
    task_runner_->RunUntilCheckpoint("ring_buffer_attached");
  }

  ProducerIPCClientImpl* client() {
    return static_cast<ProducerIPCClientImpl*>(producer_endpoint_.get());
  }

  const base::FlatSet<ProtocolAbiVersion>& protocol_abi_versions() {
    return test::ProducerIPCClientTestPeer::protocol_abi_versions(client());
  }

  void Drain() {
    std::string name = "drain_" + std::to_string(next_checkpoint_++);
    auto drained = task_runner_->CreateCheckpoint(name);
    producer_endpoint_->DrainV2RingBuffer();
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

  TraceStats GetTraceStats() {
    TraceStats result;
    auto on_stats = task_runner_->CreateCheckpoint("ring_buffer_stats");
    EXPECT_CALL(consumer_, OnTraceStats(true, _))
        .WillOnce([&](bool, const TraceStats& stats) {
          result = stats;
          on_stats();
        });
    consumer_endpoint_->GetTraceStats();
    task_runner_->RunUntilCheckpoint("ring_buffer_stats");
    return result;
  }

  static std::string Packet(const std::string& value) {
    return std::string("\xa3\x38\x0a") + static_cast<char>(value.size()) +
           value + '\x04';
  }
  std::shared_ptr<PosixSharedMemory> memory_;
  std::unique_ptr<tracing_v2::SharedRingBuffer> ring_buffer_;
  size_t next_checkpoint_ = 0;
};

struct InstanceProtocolTestCase {
  const char* name;
  std::vector<ProtocolAbiVersion> common_versions;
  uint32_t probability;
  bool v2_destination;
  std::optional<ProtocolAbiVersion> selected_version;
};

class InstanceProtocolIntegrationTest
    : public RingBufferTransportIntegrationTest,
      public testing::WithParamInterface<InstanceProtocolTestCase> {};

TEST_P(InstanceProtocolIntegrationTest, InstanceWriterUsesCommonProtocol) {
  const auto& param = GetParam();
  // Change the client's accepted set before any writers exist. The service
  // still accepts both formats, so it would expose an invalid v1 fallback.
  EXPECT_CALL(producer_, OnConnect());
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2},
      param.common_versions);

  DataSourceDescriptor descriptor;
  descriptor.set_name("perfetto.ring_buffer_endpoint");
  producer_endpoint_->RegisterDataSource(descriptor);

  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  if (param.v2_destination)
    buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* source = config.add_data_sources()->mutable_config();
  source->set_name(descriptor.name());
  source->mutable_experimental_tracing_v2()->set_use_v2_probability_percent(
      param.probability);
  source->mutable_experimental_tracing_v2()
      ->add_chunk_size_options()
      ->set_size_bytes(256);
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
        EXPECT_EQ(setup.supports_tracing_v2(), param.v2_destination);
        EXPECT_NE(setup.target_buffer(), 0u);
        writer = producer_endpoint_->CreateTraceWriter(
            target_buffer, BufferExhaustedPolicy::kDrop, instance);
        EXPECT_EQ(writer->writer_id() != 0, param.selected_version.has_value());
        auto packet = writer->NewTracePacket();
        if (param.selected_version) {
          EXPECT_EQ(
              packet->encoding() == protozero::Message::Encoding::kProtoGroup,
              param.selected_version == ProtocolAbiVersion::kV2);
        }
        packet->set_for_testing()->set_str(payload);
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([&](DataSourceInstanceID id, const DataSourceConfig&) {
        EXPECT_EQ(id, instance);
        started();
      });
  consumer_endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("instance_started");

  // The overload without an instance must also respect the common set.
  const bool supports_v1 =
      protocol_abi_versions().count(ProtocolAbiVersion::kV1);
  auto legacy = producer_endpoint_->CreateTraceWriter(
      target_buffer, BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(legacy->writer_id() != 0, supports_v1);
  if (!supports_v1)
    legacy->NewTracePacket()->set_for_testing()->set_str("forbidden v1");
  legacy.reset();

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
  ASSERT_EQ(packets.size(), param.selected_version ? 1u : 0u);
  if (param.selected_version)
    EXPECT_EQ(packets[0].for_testing().str(), payload);

  writer.reset();
  auto stopped = task_runner_->CreateCheckpoint("instance_stopped");
  EXPECT_CALL(producer_, StopDataSource(instance));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(stopped));
  consumer_endpoint_->DisableTracing();
  task_runner_->RunUntilCheckpoint("instance_stopped");
}

INSTANTIATE_TEST_SUITE_P(
    CommonVersions,
    InstanceProtocolIntegrationTest,
    testing::Values(InstanceProtocolTestCase{"BothSelectV2",
                                             {ProtocolAbiVersion::kV1,
                                              ProtocolAbiVersion::kV2},
                                             100,
                                             true,
                                             ProtocolAbiVersion::kV2},
                    InstanceProtocolTestCase{
                        "BothSelectV1",
                        {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2},
                        0,
                        true,
                        ProtocolAbiVersion::kV1},
                    InstanceProtocolTestCase{"V1Only",
                                             {ProtocolAbiVersion::kV1},
                                             100,
                                             true,
                                             ProtocolAbiVersion::kV1},
                    InstanceProtocolTestCase{"V2OnlySelected",
                                             {ProtocolAbiVersion::kV2},
                                             100,
                                             true,
                                             ProtocolAbiVersion::kV2},
                    InstanceProtocolTestCase{"V2OnlyNotSelected",
                                             {ProtocolAbiVersion::kV2},
                                             0,
                                             true,
                                             std::nullopt},
                    InstanceProtocolTestCase{
                        "BothWithV1Destination",
                        {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2},
                        100,
                        false,
                        ProtocolAbiVersion::kV1},
                    InstanceProtocolTestCase{"V2OnlyWithV1Destination",
                                             {ProtocolAbiVersion::kV2},
                                             100,
                                             false,
                                             std::nullopt}),
    [](const testing::TestParamInfo<InstanceProtocolTestCase>& info) {
      return info.param.name;
    });

TEST_F(RingBufferTransportIntegrationTest, RejectionStopsV2WithoutFallback) {
  // Attach the service's one ring buffer for this producer first. The
  // automatic attach at setup then gets a rejection.
  Attach();
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

  // The attach request went out during setup. The service replies in order, so
  // after this Sync() round trip the rejection has arrived.
  auto synced = task_runner_->CreateCheckpoint("rejection_received");
  producer_endpoint_->Sync(synced);
  task_runner_->RunUntilCheckpoint("rejection_received");

  // This writer keeps the rejected mapping. The service never reads it, so
  // its packets never reach the trace.
  early_writer->NewTracePacket()->set_for_testing()->set_str("after rejection");
  // New writers of the instance are NullTraceWriters. There is no v1
  // fallback.
  auto writer = producer_endpoint_->CreateTraceWriter(
      target, BufferExhaustedPolicy::kDrop, instance);
  EXPECT_EQ(writer->writer_id(), 0u);
  writer->NewTracePacket()->set_for_testing()->set_str("no fallback");

  // The connection stays up, and v1 writers still work.
  auto legacy = producer_endpoint_->CreateTraceWriter(
      target, BufferExhaustedPolicy::kDrop);
  legacy->NewTracePacket()->set_for_testing()->set_str("legacy");
  auto committed = task_runner_->CreateCheckpoint("legacy_after_rejection");
  legacy->Flush(committed);
  task_runner_->RunUntilCheckpoint("legacy_after_rejection");
  auto packets = Read();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "legacy");
  EXPECT_THAT(protocol_abi_versions(),
              ElementsAre(ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2));
}

// Loss that the producer flags before its first chunk has no sequence to
// mark. It is not a service discard either.
TEST_F(RingBufferTransportIntegrationTest, LossBeforeFirstRouteIsNotDiscard) {
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  auto setups = Start(config, 1);
  ASSERT_EQ(setups.size(), 1u);
  Attach();
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
        EXPECT_EQ(stats.chunks_discarded(), 0u);
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
  Attach();
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
  auto setups = Start(config, 2);
  ASSERT_EQ(setups.size(), 2u);
  EXPECT_FALSE(setups[0].supports_tracing_v2());
  EXPECT_TRUE(setups[1].supports_tracing_v2());
  Attach();

  // The producer is allowed to write to the v1 buffer, but not through the
  // ring buffer. Buffer 0 is not a buffer of this producer.
  const BufferID v1_buffer = static_cast<BufferID>(setups[0].target_buffer());
  const BufferID targets[] = {v1_buffer, 0};
  for (size_t i = 0; i < 2; ++i) {
    auto writer = tracing_v2::test::MakeWriter(
        ring_buffer_.get(), static_cast<WriterID>(i + 1), targets[i]);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("rejected")));
  }
  Drain();
  EXPECT_TRUE(Read().empty());
  // The service drops both chunks before any trace buffer sees them.
  EXPECT_EQ(GetTraceStats().chunks_discarded(), 2u);
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
  Attach();
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
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(64);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  auto setups = Start(config, 1);
  ASSERT_EQ(setups.size(), 1u);
  const auto target = static_cast<BufferID>(setups[0].target_buffer());
  Attach();

  auto writer = tracing_v2::test::MakeWriter(ring_buffer_.get(), 1, target);
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("corrupt")));
  writer.FinishCurrentChunk();
  tracing_v2::test::SharedRingBufferInternalsForTest::SetChunkStateWord(
      ring_buffer_.get(), tracing_v2::ChunkIndex::FromIndex(0),
      static_cast<uint32_t>(tracing_v2::ChunkState::kReserved5));
  Drain();

  // The service stops the reader but keeps the connection. Drain() ends with
  // a Sync() round trip, so the connection is still alive here.
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer, Packet("ignored")));
  writer.FinishCurrentChunk();
  Drain();
  EXPECT_TRUE(Read().empty());
  EXPECT_EQ(tracing_v2::test::SharedRingBufferInternalsForTest::GetReadPos(
                ring_buffer_.get()),
            0u);

  const TraceStats stats = GetTraceStats();
  ASSERT_EQ(stats.buffer_stats_size(), 1);
  EXPECT_EQ(stats.buffer_stats()[0].abi_violations(), 1u);
}

// Each advertised version is independent. Keep all common versions, and do
// not add v1 to a v2-only offer. An old producer lists none and gets v1.
TEST_F(RingBufferTransportIntegrationTest, ServiceReturnsAllCommonVersions) {
  EXPECT_THAT(protocol_abi_versions(),
              ElementsAre(ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2));

  // Returns the common list, or nullopt if the service rejects the request.
  auto negotiate = [&](std::vector<protos::gen::ProtocolAbiVersion> offered)
      -> std::optional<std::vector<protos::gen::ProtocolAbiVersion>> {
    struct Listener : public ipc::ServiceProxy::EventListener {
      std::function<void()> on_connect;
      void OnConnect() override { on_connect(); }
    } listener;
    protos::gen::ProducerPortProxy port(&listener);
    const std::string name = "raw_" + std::to_string(next_checkpoint_++);
    listener.on_connect = task_runner_->CreateCheckpoint(name + "_connected");
    auto ipc_client = ipc::Client::CreateInstance(
        {kProducerSock.name(), /*sock_retry=*/false}, task_runner_.get());
    ipc_client->BindService(port.GetWeakPtr());
    task_runner_->RunUntilCheckpoint(name + "_connected");

    protos::gen::InitializeConnectionRequest req;
    req.set_producer_name(name);
    for (auto version : offered)
      req.add_supported_protocol_abi_versions(version);
    std::optional<std::vector<protos::gen::ProtocolAbiVersion>> common;
    auto replied = task_runner_->CreateCheckpoint(name + "_replied");
    ipc::Deferred<protos::gen::InitializeConnectionResponse> reply;
    reply.Bind(
        [&](ipc::AsyncResult<protos::gen::InitializeConnectionResponse> resp) {
          if (resp)
            common = resp->protocol_abi_versions();
          replied();
        });
    port.InitializeConnection(req, std::move(reply));
    task_runner_->RunUntilCheckpoint(name + "_replied");
    return common;
  };
  using protos::gen::PROTOCOL_ABI_VERSION_V1;
  using protos::gen::PROTOCOL_ABI_VERSION_V2;
  const auto kV3 = static_cast<protos::gen::ProtocolAbiVersion>(3);
  using Versions = std::vector<protos::gen::ProtocolAbiVersion>;
  EXPECT_EQ(negotiate({PROTOCOL_ABI_VERSION_V1, PROTOCOL_ABI_VERSION_V2, kV3}),
            (Versions{PROTOCOL_ABI_VERSION_V1, PROTOCOL_ABI_VERSION_V2}));
  EXPECT_EQ(negotiate({PROTOCOL_ABI_VERSION_V2}),
            Versions{PROTOCOL_ABI_VERSION_V2});
  EXPECT_EQ(negotiate({PROTOCOL_ABI_VERSION_V1}),
            Versions{PROTOCOL_ABI_VERSION_V1});
  EXPECT_EQ(negotiate({}), Versions{PROTOCOL_ABI_VERSION_V1});
  EXPECT_EQ(negotiate({PROTOCOL_ABI_VERSION_V2, PROTOCOL_ABI_VERSION_V1,
                       PROTOCOL_ABI_VERSION_V2}),
            (Versions{PROTOCOL_ABI_VERSION_V1, PROTOCOL_ABI_VERSION_V2}));
  EXPECT_EQ(negotiate({protos::gen::PROTOCOL_ABI_VERSION_UNSPECIFIED}),
            std::nullopt);
  EXPECT_EQ(negotiate({kV3, PROTOCOL_ABI_VERSION_V2}),
            Versions{PROTOCOL_ABI_VERSION_V2});
  EXPECT_EQ(negotiate({kV3}), std::nullopt);
}

// A reply must contain only offered versions.
TEST_F(RingBufferTransportIntegrationTest,
       DisconnectsIfServiceReturnsUnknownVersion) {
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2},
      {ProtocolAbiVersion::kV2, static_cast<ProtocolAbiVersion>(3)});
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  producer_endpoint_.reset();
}

TEST_F(RingBufferTransportIntegrationTest,
       DisconnectsIfServiceAddsV1ToV2OnlyOffer) {
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV2},
      {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2});
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  producer_endpoint_.reset();
}

TEST_F(RingBufferTransportIntegrationTest, V2OnlyOfferRejectsLegacyService) {
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV2}, {});
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  producer_endpoint_.reset();
}

TEST_F(RingBufferTransportIntegrationTest, LegacyServicePermitsOnlyV1) {
  EXPECT_CALL(producer_, OnConnect());
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2}, {});
  EXPECT_THAT(protocol_abi_versions(), ElementsAre(ProtocolAbiVersion::kV1));
}

TEST_F(RingBufferTransportIntegrationTest, ClientKeepsOnlyCommonVersions) {
  EXPECT_CALL(producer_, OnConnect());
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2},
      {ProtocolAbiVersion::kV2, ProtocolAbiVersion::kV2});
  EXPECT_THAT(protocol_abi_versions(), ElementsAre(ProtocolAbiVersion::kV2));
}

TEST_F(RingBufferTransportIntegrationTest, ClientAcceptsV2OnlyOffer) {
  EXPECT_CALL(producer_, OnConnect());
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV2}, {ProtocolAbiVersion::kV2});
  EXPECT_THAT(protocol_abi_versions(), ElementsAre(ProtocolAbiVersion::kV2));
}

TEST_F(RingBufferTransportIntegrationTest, RejectsV2WithShmemEmulation) {
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2},
      {ProtocolAbiVersion::kV2}, /*use_shmem_emulation=*/true);
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  producer_endpoint_.reset();
}

TEST_F(RingBufferTransportIntegrationTest, RejectedInitializationDisconnects) {
  auto disconnected = task_runner_->CreateCheckpoint("producer_disconnected");
  EXPECT_CALL(producer_, OnDisconnect()).WillOnce(disconnected);
  test::ProducerIPCClientTestPeer::OnConnectionInitialized(
      client(), {ProtocolAbiVersion::kV2}, {}, /*use_shmem_emulation=*/false,
      /*connection_succeeded=*/false);
  task_runner_->RunUntilCheckpoint("producer_disconnected");
  producer_endpoint_.reset();
}

TEST_F(RingBufferTransportIntegrationTest, EarlyPublicationAndDrain) {
  ASSERT_THAT(protocol_abi_versions(),
              ElementsAre(ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2));
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
        EXPECT_TRUE(cfg.supports_tracing_v2());
        target = static_cast<BufferID>(cfg.target_buffer());
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce(InvokeWithoutArgs(started));
  consumer_endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("ring_buffer_started");

  std::shared_ptr<PosixSharedMemory> memory =
      PosixSharedMemory::Create(sizeof(tracing_v2::RingBufferHeader) + 4 * 256);
  tracing_v2::SharedRingBuffer ring_buffer(
      static_cast<uint8_t*>(memory->start()), memory->size(), 256);
  auto writer = tracing_v2::test::MakeWriter(&ring_buffer, 2, target);
  // Field 900 contains TestEvent.str, with the internal bare closing marker.
  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer,
                                              "\xa3\x38\x0a\x05"
                                              "early\x04"));
  writer.FinishCurrentChunk();
  auto attached = task_runner_->CreateCheckpoint("ring_buffer_attached");
  producer_endpoint_->AttachV2RingBuffer(memory, 256, [attached](bool success) {
    EXPECT_TRUE(success);
    attached();
  });
  task_runner_->RunUntilCheckpoint("ring_buffer_attached");
  EXPECT_EQ(tracing_v2::test::SharedRingBufferInternalsForTest::GetReadPos(
                &ring_buffer),
            1u);

  ASSERT_TRUE(tracing_v2::test::WriteFragment(&writer,
                                              "\xa3\x38\x0a\x05"
                                              "later\x04"));
  writer.FinishCurrentChunk();
  auto drained = task_runner_->CreateCheckpoint("ring_buffer_drained");
  producer_endpoint_->DrainV2RingBuffer();
  producer_endpoint_->Sync([&] {
    EXPECT_EQ(tracing_v2::test::SharedRingBufferInternalsForTest::GetReadPos(
                  &ring_buffer),
              2u);
    drained();
  });
  task_runner_->RunUntilCheckpoint("ring_buffer_drained");

  // The manual ring buffer writer uses ID 2. The first v1 writer receives ID
  // 1.
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
                             .has_supports_tracing_v2());
        }
        if (!more)
          read();
      });
  consumer_endpoint_->ReadBuffers();
  task_runner_->RunUntilCheckpoint("ring_buffer_read");
  EXPECT_THAT(values,
              testing::UnorderedElementsAre("early", "later", "legacy"));
}

TEST_F(RingBufferTransportIntegrationTest,
       RejectsInvalidAndDuplicateRingBuffers) {
  auto attach = [&](size_t size, uint32_t chunk_size, bool accepted,
                    const char* checkpoint) {
    std::shared_ptr<PosixSharedMemory> memory = PosixSharedMemory::Create(size);
    auto done = task_runner_->CreateCheckpoint(checkpoint);
    producer_endpoint_->AttachV2RingBuffer(memory, chunk_size,
                                           [=](bool result) {
                                             EXPECT_EQ(result, accepted);
                                             done();
                                           });
    task_runner_->RunUntilCheckpoint(checkpoint);
  };
  // An invalid layout does not persist state on the peer. A second attach with
  // a valid layout can still succeed.
  attach(4096, 256, false, "invalid_layout");
  attach(sizeof(tracing_v2::RingBufferHeader) + 1024, 256, true, "first_valid");
  // The service attaches one ring buffer per producer. A later one is
  // rejected.
  attach(sizeof(tracing_v2::RingBufferHeader) + 1024, 256, false,
         "duplicate_ring_buffer");
  auto synced = task_runner_->CreateCheckpoint("still_connected");
  producer_endpoint_->Sync(synced);
  task_runner_->RunUntilCheckpoint("still_connected");
  EXPECT_THAT(protocol_abi_versions(),
              ElementsAre(ProtocolAbiVersion::kV1, ProtocolAbiVersion::kV2));
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
