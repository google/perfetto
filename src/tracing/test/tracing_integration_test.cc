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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
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

#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::_;

constexpr char kProducerSockName[] = TEST_SOCK_NAME("tracing_test-producer");
constexpr char kConsumerSockName[] = TEST_SOCK_NAME("tracing_test-consumer");

class TracingIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
    DESTROY_TEST_SOCK(kProducerSockName);
    DESTROY_TEST_SOCK(kConsumerSockName);
    task_runner_.reset(new base::TestTaskRunner());
  }

  void TearDown() override {
    task_runner_.reset();
    DESTROY_TEST_SOCK(kProducerSockName);
    DESTROY_TEST_SOCK(kConsumerSockName);
  }

  std::unique_ptr<base::TestTaskRunner> task_runner_;
};

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
};

class MockConsumer : public Consumer {
 public:
  ~MockConsumer() override {}

  // Producer implementation.
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD2(OnTracePackets, void(std::vector<TracePacket>*, bool));

  // Workaround, gmock doesn't support yet move-only types, passing a pointer.
  void OnTraceData(std::vector<TracePacket> packets, bool has_more) {
    OnTracePackets(&packets, has_more);
  }
};

TEST_F(TracingIntegrationTest, WithIPCTransport) {
  // Create the service host.
  std::unique_ptr<ServiceIPCHost> svc =
      ServiceIPCHost::CreateInstance(task_runner_.get());
  svc->Start(kProducerSockName, kConsumerSockName);

  // Create and connect a Producer.
  MockProducer producer;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
      ProducerIPCClient::Connect(kProducerSockName, &producer,
                                 task_runner_.get());
  auto on_producer_connect =
      task_runner_->CreateCheckpoint("on_producer_connect");
  EXPECT_CALL(producer, OnConnect()).WillOnce(Invoke(on_producer_connect));
  task_runner_->RunUntilCheckpoint("on_producer_connect");

  // Register a data source.
  DataSourceDescriptor ds_desc;
  ds_desc.set_name("perfetto.test");
  auto on_data_source_registered =
      task_runner_->CreateCheckpoint("on_data_source_registered");
  producer_endpoint->RegisterDataSource(
      ds_desc, [on_data_source_registered](DataSourceID dsid) {
        PERFETTO_DLOG("Registered data source with ID: %" PRIu64, dsid);
        on_data_source_registered();
      });
  task_runner_->RunUntilCheckpoint("on_data_source_registered");

  // Create and connect a Consumer.
  MockConsumer consumer;
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint =
      ConsumerIPCClient::Connect(kConsumerSockName, &consumer,
                                 task_runner_.get());
  auto on_consumer_connect =
      task_runner_->CreateCheckpoint("on_consumer_connect");
  EXPECT_CALL(consumer, OnConnect()).WillOnce(Invoke(on_consumer_connect));
  task_runner_->RunUntilCheckpoint("on_consumer_connect");

  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  consumer_endpoint->EnableTracing(trace_config);

  // At this point, the Producer should be asked to turn its data source on.
  DataSourceInstanceID ds_iid = 0;

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer, CreateDataSourceInstance(_, _))
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
      producer_endpoint->CreateTraceWriter(global_buf_id);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[8];
    sprintf(buf, "evt_%zu", i);
    writer->NewTracePacket()->set_for_testing()->set_str(buf, strlen(buf));
  }

  // Allow the service to see the CommitData() before disabling tracing.
  auto on_data_committed = task_runner_->CreateCheckpoint("on_data_committed");
  writer->Flush(on_data_committed);
  task_runner_->RunUntilCheckpoint("on_data_committed");
  writer.reset();

  // Disable tracing.
  consumer_endpoint->DisableTracing();
  auto on_teardown_ds_instance =
      task_runner_->CreateCheckpoint("on_teardown_ds_instance");
  EXPECT_CALL(producer, TearDownDataSourceInstance(ds_iid))
      .WillOnce(InvokeWithoutArgs(on_teardown_ds_instance));
  task_runner_->RunUntilCheckpoint("on_teardown_ds_instance");

  // Read the log buffer.
  consumer_endpoint->ReadBuffers();
  size_t num_pack_rx = 0;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer, OnTracePackets(_, _))
      .WillRepeatedly(
          Invoke([&num_pack_rx, all_packets_rx](
                     std::vector<TracePacket>* packets, bool has_more) {
            // TODO(primiano): check contents, requires both pblite and pzero.
            num_pack_rx += packets->size();
            if (!has_more)
              all_packets_rx();
          }));
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  ASSERT_EQ(kNumPackets, num_pack_rx);

  // TODO(primiano): cover FreeBuffers.

  // Destroy the service and check that both Producer and Consumer see an
  // OnDisconnect() call.

  auto on_producer_disconnect =
      task_runner_->CreateCheckpoint("on_producer_disconnect");
  EXPECT_CALL(producer, OnDisconnect())
      .WillOnce(Invoke(on_producer_disconnect));

  auto on_consumer_disconnect =
      task_runner_->CreateCheckpoint("on_consumer_disconnect");
  EXPECT_CALL(consumer, OnDisconnect())
      .WillOnce(Invoke(on_consumer_disconnect));

  svc.reset();
  task_runner_->RunUntilCheckpoint("on_producer_disconnect");
  task_runner_->RunUntilCheckpoint("on_consumer_disconnect");
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
