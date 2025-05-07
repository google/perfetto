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
#include "src/tracing/service/tracing_service_impl.h"
#include "test/gtest_and_gmock.h"

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
using testing::Invoke;
using testing::InvokeWithoutArgs;

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
    EXPECT_CALL(producer_, OnConnect()).WillOnce(Invoke(on_producer_connect));
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
    EXPECT_CALL(consumer_, OnConnect()).WillOnce(Invoke(on_consumer_connect));
    task_runner_->RunUntilCheckpoint("on_consumer_connect");

    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&producer_));
    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&consumer_));
  }

  void TearDown() override {
    // Destroy the service and check that both Producer and Consumer see an
    // OnDisconnect() call.

    auto on_producer_disconnect =
        task_runner_->CreateCheckpoint("on_producer_disconnect");
    EXPECT_CALL(producer_, OnDisconnect())
        .WillOnce(Invoke(on_producer_disconnect));

    auto on_consumer_disconnect =
        task_runner_->CreateCheckpoint("on_consumer_disconnect");
    EXPECT_CALL(consumer_, OnDisconnect())
        .WillOnce(Invoke(on_consumer_disconnect));

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
      .WillOnce(
          Invoke([&setup_id, &setup_cfg_proto](DataSourceInstanceID id,
                                               const DataSourceConfig& cfg) {
            setup_id = id;
            setup_cfg_proto = cfg;
          }));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce(
          Invoke([on_create_ds_instance, &ds_iid, &global_buf_id, &setup_id,
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
          }));
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
      .WillRepeatedly(
          Invoke([&num_pack_rx, all_packets_rx, &trace_config,
                  &saw_clock_snapshot, &saw_trace_config, &saw_trace_stats](
                     std::vector<TracePacket>* packets, bool has_more) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
            const int kExpectedMinNumberOfClocks = 1;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
            const int kExpectedMinNumberOfClocks = 2;
#else
            const int kExpectedMinNumberOfClocks = 6;
#endif

            for (auto& encoded_packet : *packets) {
              protos::gen::TracePacket packet;
              ASSERT_TRUE(packet.ParseFromString(
                  encoded_packet.GetRawBytesForTesting()));
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
          }));
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
      .WillOnce(Invoke([](const std::string& err) {
        EXPECT_THAT(err,
                    testing::HasSubstr("EnableTracing IPC request rejected"));
      }));

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
      .WillOnce(Invoke([on_create_ds_instance, &global_buf_id](
                           DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      }));
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
      .WillOnce(Invoke([on_create_ds_instance, &global_buf_id](
                           DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      }));
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
      .WillOnce(Invoke([this](FlushRequestID flush_req_id,
                              const DataSourceInstanceID*, size_t, FlushFlags) {
        producer_endpoint_->NotifyFlushComplete(flush_req_id);
      }));
  task_runner_->RunUntilCheckpoint("on_flush_complete");

  // Read the log buffer. We should see all the packets.
  consumer_endpoint_->ReadBuffers();

  size_t num_test_pack_rx = 0;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly(
          Invoke([&num_test_pack_rx, all_packets_rx](
                     std::vector<TracePacket>* packets, bool has_more) {
            for (auto& encoded_packet : *packets) {
              protos::gen::TracePacket packet;
              ASSERT_TRUE(packet.ParseFromString(
                  encoded_packet.GetRawBytesForTesting()));
              if (packet.has_for_testing()) {
                num_test_pack_rx++;
              }
            }
            if (!has_more)
              all_packets_rx();
          }));
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
