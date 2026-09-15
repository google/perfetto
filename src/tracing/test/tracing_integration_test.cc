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

#include "perfetto/base/build_config.h"
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
#include "src/tracing/service/tracing_service_impl.h"
#include "src/tracing/v2/producer_ring.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "test/gtest_and_gmock.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include "src/tracing/ipc/posix_shared_memory.h"
#endif

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

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Forwards a producer ProducerRing's data notifications to its IPC endpoint. In
// production this adapter lives in the muxer; here it drives the direct v2 path
// through the real IPC client.
class EndpointServiceChannel : public tracing_v2::ProducerRing::ServiceChannel {
 public:
  explicit EndpointServiceChannel(TracingService::ProducerEndpoint* endpoint)
      : endpoint_(endpoint) {}
  void NotifyRingData(std::function<void()> on_picked_up) override {
    endpoint_->NotifyTracingV2RingData(std::move(on_picked_up));
  }

 private:
  TracingService::ProducerEndpoint* const endpoint_;
};
#endif  // !PERFETTO_OS_WIN

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
    EXPECT_CALL(producer_, OnDisconnect()).WillOnce(on_producer_disconnect);

    auto on_consumer_disconnect =
        task_runner_->CreateCheckpoint("on_consumer_disconnect");
    EXPECT_CALL(consumer_, OnDisconnect()).WillOnce(on_consumer_disconnect);

    svc_.reset();
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

class TracingIntegrationTestWithChunkSize
    : public TracingIntegrationTest,
      public testing::WithParamInterface<uint32_t> {};

TEST_P(TracingIntegrationTestWithChunkSize, WithIPCTransport) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  if (GetParam() != 0) {
    auto* producer_config = trace_config.add_producers();
    producer_config->set_producer_name("perfetto.mock_producer");
    producer_config->set_tracing_v2_chunk_size_bytes(GetParam());
  }
  EXPECT_EQ(producer_endpoint_->tracing_v2_chunk_size_bytes(), 0u);
  consumer_endpoint_->EnableTracing(trace_config);

  // At this point, the Producer should be asked to turn its data source on.
  DataSourceInstanceID ds_iid = 0;

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup()).WillOnce([this] {
    EXPECT_EQ(producer_endpoint_->tracing_v2_chunk_size_bytes(), GetParam());
  });

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

// Zero leaves the field absent. Valid explicit values reach the producer even
// if it only creates v1 writers. The 260-byte case verifies that chunk sizes
// need not be powers of two.
INSTANTIATE_TEST_SUITE_P(ChunkSize,
                         TracingIntegrationTestWithChunkSize,
                         testing::Values(0u, 1024u, 260u));

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Exercises the direct v2 ring transport across a real IPC socket: the producer
// allocates a sealed memfd ring, adopts it over IPC (real FD passing), writes
// v2 packets including one that fragments across many chunks (>128 KiB), and
// the service reads the ring directly and delivers the packets to the consumer.
TEST_F(TracingIntegrationTest, TracingV2DirectRingOverIPC) {
  // The service advertised direct v2 transport support at connection setup.
  ASSERT_TRUE(producer_endpoint_->IsTracingV2DirectTransportSupported());

  TraceConfig trace_config;
  auto* buffer = trace_config.add_buffers();
  buffer->set_size_kb(4096);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  consumer_endpoint_->EnableTracing(trace_config);

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
  ASSERT_NE(global_buf_id, 0u);

  // The producer allocates its own ring in a sealed memfd and hands it to the
  // service over IPC. 128 chunks of 4 KiB (512 KiB) comfortably hold the large
  // packet below without the single-threaded writer having to stall.
  constexpr uint32_t kNumChunks = 128;
  constexpr uint32_t kChunkSize = 4096;
  auto shmem = std::shared_ptr<SharedMemory>(
      PosixSharedMemory::Create(static_cast<size_t>(
          tracing_v2::RingLogicalSize(kNumChunks, kChunkSize))));
  ASSERT_TRUE(shmem);
  EndpointServiceChannel channel(producer_endpoint_.get());
  auto ring =
      tracing_v2::ProducerRing::Create(shmem, kNumChunks, kChunkSize, &channel);
  ASSERT_TRUE(ring);
  TracingService::ProducerEndpoint::AdoptTracingV2RingArgs adopt_args;
  adopt_args.shared_memory = shmem;
  adopt_args.num_chunks = kNumChunks;
  adopt_args.chunk_size = kChunkSize;
  bool ring_adopted = false;
  producer_endpoint_->AdoptTracingV2Ring(
      std::move(adopt_args),
      [&ring_adopted](bool accepted) { ring_adopted = accepted; });

  std::unique_ptr<TraceWriter> writer =
      ring->CreateTraceWriter(global_buf_id, BufferExhaustedPolicy::kDrop);
  ASSERT_TRUE(writer);
  writer->NewTracePacket()->set_for_testing()->set_str("small_before");
  const std::string big(160 * 1024,
                        'x');  // > 128 KiB: fragments across chunks.
  writer->NewTracePacket()->set_for_testing()->set_str(big);
  writer->NewTracePacket()->set_for_testing()->set_str("small_after");

  // Flush hands the data to the service over IPC (NotifyTracingV2RingData),
  // which drains the ring and replies; the flush callback fires on that reply.
  auto on_flush = task_runner_->CreateCheckpoint("on_flush");
  writer->Flush(on_flush);
  task_runner_->RunUntilCheckpoint("on_flush");

  // The adoption RPC resolved before the flush drained the ring.
  EXPECT_TRUE(ring_adopted);

  consumer_endpoint_->ReadBuffers();
  bool saw_small_before = false, saw_big = false, saw_small_after = false;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&](std::vector<TracePacket>* packets, bool has_more) {
        for (auto& encoded_packet : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(
              packet.ParseFromString(encoded_packet.GetRawBytesForTesting()));
          if (!packet.has_for_testing())
            continue;
          const std::string& s = packet.for_testing().str();
          if (s == "small_before")
            saw_small_before = true;
          else if (s == "small_after")
            saw_small_after = true;
          else if (s == big)
            saw_big = true;
        }
        if (!has_more)
          all_packets_rx();
      });
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  EXPECT_TRUE(saw_small_before);
  EXPECT_TRUE(saw_big);
  EXPECT_TRUE(saw_small_after);

  consumer_endpoint_->DisableTracing();
  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(producer_, StopDataSource(_));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
}
#endif  // !PERFETTO_OS_WIN

TEST_F(TracingIntegrationTest, InvalidTracingV2ChunkSizeRejectsConfig) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  auto* producer_config = trace_config.add_producers();
  producer_config->set_producer_name("perfetto.mock_producer");
  producer_config->set_tracing_v2_chunk_size_bytes(255);

  // Invalid producer settings reject the config even for v1 data sources.
  // Wait for that rejection, rather than for a data source that cannot start.
  EXPECT_CALL(producer_, OnTracingSetup()).Times(0);
  EXPECT_CALL(producer_, SetupDataSource(_, _)).Times(0);
  EXPECT_CALL(producer_, StartDataSource(_, _)).Times(0);
  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(consumer_, OnTracingDisabled(
                             testing::HasSubstr("tracing_v2_chunk_size_bytes")))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  consumer_endpoint_->EnableTracing(trace_config);
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
  EXPECT_EQ(producer_endpoint_->shared_memory(), nullptr);
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
