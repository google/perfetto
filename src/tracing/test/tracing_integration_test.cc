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

#include <inttypes.h>
#include <unistd.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"
#include "perfetto/tracing/ipc/service_ipc_host.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"

#include "perfetto/config/trace_config.pb.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::_;

constexpr char kProducerSockName[] = TEST_SOCK_NAME("tracing_test-producer");
constexpr char kConsumerSockName[] = TEST_SOCK_NAME("tracing_test-consumer");

class MockProducer : public Producer {
 public:
  ~MockProducer() override {}

  // Producer implementation.
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD2(CreateDataSourceInstance,
               void(DataSourceInstanceID, const DataSourceConfig&));
  MOCK_METHOD1(TearDownDataSourceInstance, void(DataSourceInstanceID));
  MOCK_METHOD0(uid, uid_t());
  MOCK_METHOD0(OnTracingSetup, void());
  MOCK_METHOD3(Flush,
               void(FlushRequestID, const DataSourceInstanceID*, size_t));
};

class MockConsumer : public Consumer {
 public:
  ~MockConsumer() override {}

  // Producer implementation.
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD0(OnTracingDisabled, void());
  MOCK_METHOD2(OnTracePackets, void(std::vector<TracePacket>*, bool));

  // Workaround, gmock doesn't support yet move-only types, passing a pointer.
  void OnTraceData(std::vector<TracePacket> packets, bool has_more) {
    OnTracePackets(&packets, has_more);
  }
};

void CheckTraceStats(const protos::TracePacket& packet) {
  EXPECT_TRUE(packet.has_trace_stats());
  EXPECT_GE(packet.trace_stats().producers_seen(), 1u);
  EXPECT_EQ(1u, packet.trace_stats().producers_connected());
  EXPECT_EQ(1u, packet.trace_stats().data_sources_registered());
  EXPECT_EQ(1u, packet.trace_stats().tracing_sessions());
  EXPECT_EQ(1u, packet.trace_stats().total_buffers());
  EXPECT_EQ(1, packet.trace_stats().buffer_stats_size());

  const auto& buf_stats = packet.trace_stats().buffer_stats(0);
  EXPECT_GT(buf_stats.bytes_written(), 0u);
  EXPECT_GT(buf_stats.chunks_written(), 0u);
  EXPECT_EQ(0u, buf_stats.chunks_overwritten());
  EXPECT_EQ(0u, buf_stats.write_wrap_count());
  EXPECT_EQ(0u, buf_stats.patches_failed());
  EXPECT_EQ(0u, buf_stats.readaheads_failed());
  EXPECT_EQ(0u, buf_stats.abi_violations());
}

class TracingIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
    DESTROY_TEST_SOCK(kProducerSockName);
    DESTROY_TEST_SOCK(kConsumerSockName);
    task_runner_.reset(new base::TestTaskRunner());

    // Create the service host.
    svc_ = ServiceIPCHost::CreateInstance(task_runner_.get());
    svc_->Start(kProducerSockName, kConsumerSockName);

    // Create and connect a Producer.
    producer_endpoint_ = ProducerIPCClient::Connect(
        kProducerSockName, &producer_, "perfetto.mock_producer",
        task_runner_.get());
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
        kConsumerSockName, &consumer_, task_runner_.get());
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
    DESTROY_TEST_SOCK(kProducerSockName);
    DESTROY_TEST_SOCK(kConsumerSockName);
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
  EXPECT_CALL(producer_, CreateDataSourceInstance(_, _))
      .WillOnce(
          Invoke([on_create_ds_instance, &ds_iid, &global_buf_id](
                     DataSourceInstanceID id, const DataSourceConfig& cfg) {
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
  std::unique_ptr<TraceWriter> writer =
      producer_endpoint_->CreateTraceWriter(global_buf_id);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[16];
    sprintf(buf, "evt_%zu", i);
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
#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
            const int kExpectedMinNumberOfClocks = 1;
#else
            const int kExpectedMinNumberOfClocks = 6;
#endif

            for (auto& encoded_packet : *packets) {
              protos::TracePacket packet;
              ASSERT_TRUE(encoded_packet.Decode(&packet));
              if (packet.has_for_testing()) {
                char buf[8];
                sprintf(buf, "evt_%zu", num_pack_rx++);
                EXPECT_EQ(std::string(buf), packet.for_testing().str());
              } else if (packet.has_clock_snapshot()) {
                EXPECT_GE(packet.clock_snapshot().clocks_size(),
                          kExpectedMinNumberOfClocks);
                saw_clock_snapshot = true;
              } else if (packet.has_trace_config()) {
                protos::TraceConfig config_proto;
                trace_config.ToProto(&config_proto);
                Slice expected_slice = Slice::Allocate(
                    static_cast<size_t>(config_proto.ByteSize()));
                config_proto.SerializeWithCachedSizesToArray(
                    expected_slice.own_data());
                Slice actual_slice = Slice::Allocate(
                    static_cast<size_t>(packet.trace_config().ByteSize()));
                packet.trace_config().SerializeWithCachedSizesToArray(
                    actual_slice.own_data());
                EXPECT_EQ(std::string(reinterpret_cast<const char*>(
                                          expected_slice.own_data()),
                                      expected_slice.size),
                          std::string(reinterpret_cast<const char*>(
                                          actual_slice.own_data()),
                                      actual_slice.size));
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
  EXPECT_CALL(producer_, TearDownDataSourceInstance(_));
  EXPECT_CALL(consumer_, OnTracingDisabled())
      .WillOnce(Invoke(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
}

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
  EXPECT_CALL(producer_, CreateDataSourceInstance(_, _))
      .WillOnce(Invoke([on_create_ds_instance, &global_buf_id](
                           DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      }));
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  std::unique_ptr<TraceWriter> writer =
      producer_endpoint_->CreateTraceWriter(global_buf_id);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[16];
    sprintf(buf, "evt_%zu", i);
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
  EXPECT_CALL(producer_, TearDownDataSourceInstance(_));
  EXPECT_CALL(consumer_, OnTracingDisabled())
      .WillOnce(Invoke(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");

  // Check that |tmp_file| contains a valid trace.proto message.
  ASSERT_EQ(0, lseek(tmp_file.fd(), 0, SEEK_SET));
  char tmp_buf[1024];
  ssize_t rsize = read(tmp_file.fd(), tmp_buf, sizeof(tmp_buf));
  ASSERT_GT(rsize, 0);
  protos::Trace tmp_trace;
  ASSERT_TRUE(tmp_trace.ParseFromArray(tmp_buf, static_cast<int>(rsize)));
  size_t num_test_packet = 0;
  bool saw_trace_stats = false;
  for (int i = 0; i < tmp_trace.packet_size(); i++) {
    const protos::TracePacket& packet = tmp_trace.packet(i);
    if (packet.has_for_testing()) {
      ASSERT_EQ("evt_" + std::to_string(num_test_packet++),
                packet.for_testing().str());
    } else if (packet.has_trace_stats()) {
      saw_trace_stats = true;
      CheckTraceStats(packet);
    }
  }
  ASSERT_TRUE(saw_trace_stats);
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

}  // namespace
}  // namespace perfetto
