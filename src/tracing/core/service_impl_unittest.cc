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

#include "src/tracing/core/service_impl.h"

#include <string.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/file_utils.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/test/test_shared_memory.h"

#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Mock;

namespace {

class MockProducer : public Producer {
 public:
  ~MockProducer() override {}

  // Producer implementation.
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD2(CreateDataSourceInstance,
               void(DataSourceInstanceID, const DataSourceConfig&));
  MOCK_METHOD1(TearDownDataSourceInstance, void(DataSourceInstanceID));
  MOCK_METHOD0(OnTracingStart, void());
  MOCK_METHOD0(OnTracingStop, void());
};

class MockConsumer : public Consumer {
 public:
  ~MockConsumer() override {}

  // Consumer implementation.
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());
  MOCK_METHOD0(OnTracingStop, void());

  void OnTraceData(std::vector<TracePacket> packets, bool has_more) override {}
};

}  // namespace

class ServiceImplTest : public testing::Test {
 public:
  ServiceImplTest() {
    auto shm_factory =
        std::unique_ptr<SharedMemory::Factory>(new TestSharedMemory::Factory());
    svc.reset(static_cast<ServiceImpl*>(
        Service::CreateInstance(std::move(shm_factory), &task_runner)
            .release()));
  }

  base::TestTaskRunner task_runner;
  std::unique_ptr<ServiceImpl> svc;
};

TEST_F(ServiceImplTest, RegisterAndUnregister) {
  MockProducer mock_producer_1;
  MockProducer mock_producer_2;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint_1 =
      svc->ConnectProducer(&mock_producer_1, 123u /* uid */);
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint_2 =
      svc->ConnectProducer(&mock_producer_2, 456u /* uid */);

  ASSERT_TRUE(producer_endpoint_1);
  ASSERT_TRUE(producer_endpoint_2);

  InSequence seq;
  EXPECT_CALL(mock_producer_1, OnConnect());
  EXPECT_CALL(mock_producer_2, OnConnect());
  task_runner.RunUntilIdle();

  ASSERT_EQ(2u, svc->num_producers());
  ASSERT_EQ(producer_endpoint_1.get(), svc->GetProducer(1));
  ASSERT_EQ(producer_endpoint_2.get(), svc->GetProducer(2));
  ASSERT_EQ(123u, svc->GetProducer(1)->uid_);
  ASSERT_EQ(456u, svc->GetProducer(2)->uid_);

  DataSourceDescriptor ds_desc1;
  ds_desc1.set_name("foo");
  producer_endpoint_1->RegisterDataSource(
      ds_desc1, [this, &producer_endpoint_1](DataSourceID id) {
        EXPECT_EQ(1u, id);
        task_runner.PostTask(
            std::bind(&Service::ProducerEndpoint::UnregisterDataSource,
                      producer_endpoint_1.get(), id));
      });

  DataSourceDescriptor ds_desc2;
  ds_desc2.set_name("bar");
  producer_endpoint_2->RegisterDataSource(
      ds_desc2, [this, &producer_endpoint_2](DataSourceID id) {
        EXPECT_EQ(1u, id);
        task_runner.PostTask(
            std::bind(&Service::ProducerEndpoint::UnregisterDataSource,
                      producer_endpoint_2.get(), id));
      });

  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_producer_1, OnDisconnect());
  producer_endpoint_1.reset();
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(&mock_producer_1);

  ASSERT_EQ(1u, svc->num_producers());
  ASSERT_EQ(nullptr, svc->GetProducer(1));

  EXPECT_CALL(mock_producer_2, OnDisconnect());
  producer_endpoint_2.reset();
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(&mock_producer_2);

  ASSERT_EQ(0u, svc->num_producers());
}

TEST_F(ServiceImplTest, EnableAndDisableTracing) {
  MockProducer mock_producer;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
      svc->ConnectProducer(&mock_producer, 123u /* uid */);
  MockConsumer mock_consumer;
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint =
      svc->ConnectConsumer(&mock_consumer);

  InSequence seq;
  EXPECT_CALL(mock_producer, OnConnect());
  EXPECT_CALL(mock_consumer, OnConnect());
  task_runner.RunUntilIdle();

  DataSourceDescriptor ds_desc;
  ds_desc.set_name("foo");
  producer_endpoint->RegisterDataSource(ds_desc, [](DataSourceID) {});

  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_producer, CreateDataSourceInstance(_, _));
  EXPECT_CALL(mock_producer, TearDownDataSourceInstance(_));
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("foo");
  ds_config->set_target_buffer(0);
  consumer_endpoint->EnableTracing(trace_config);
  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_producer, OnDisconnect());
  EXPECT_CALL(mock_consumer, OnDisconnect());
  consumer_endpoint->DisableTracing();
  producer_endpoint.reset();
  consumer_endpoint.reset();
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(&mock_producer);
  Mock::VerifyAndClearExpectations(&mock_consumer);
}

TEST_F(ServiceImplTest, LockdownMode) {
  MockConsumer mock_consumer;
  EXPECT_CALL(mock_consumer, OnConnect());
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint =
      svc->ConnectConsumer(&mock_consumer);

  TraceConfig trace_config;
  trace_config.set_lockdown_mode(
      TraceConfig::LockdownModeOperation::LOCKDOWN_SET);
  consumer_endpoint->EnableTracing(trace_config);
  task_runner.RunUntilIdle();

  InSequence seq;

  MockProducer mock_producer;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
      svc->ConnectProducer(&mock_producer, geteuid() + 1 /* uid */);

  MockProducer mock_producer_sameuid;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint_sameuid =
      svc->ConnectProducer(&mock_producer_sameuid, geteuid() /* uid */);

  EXPECT_CALL(mock_producer, OnConnect()).Times(0);
  EXPECT_CALL(mock_producer_sameuid, OnConnect());
  task_runner.RunUntilIdle();

  Mock::VerifyAndClearExpectations(&mock_producer);

  consumer_endpoint->DisableTracing();
  task_runner.RunUntilIdle();

  trace_config.set_lockdown_mode(
      TraceConfig::LockdownModeOperation::LOCKDOWN_CLEAR);
  consumer_endpoint->EnableTracing(trace_config);
  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_producer_sameuid, OnDisconnect());
  EXPECT_CALL(mock_producer, OnConnect());
  producer_endpoint_sameuid =
      svc->ConnectProducer(&mock_producer, geteuid() + 1);

  EXPECT_CALL(mock_producer, OnDisconnect());
  task_runner.RunUntilIdle();
}

TEST_F(ServiceImplTest, DisconnectConsumerWhileTracing) {
  MockProducer mock_producer;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
      svc->ConnectProducer(&mock_producer, 123u /* uid */);
  MockConsumer mock_consumer;
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint =
      svc->ConnectConsumer(&mock_consumer);

  InSequence seq;
  EXPECT_CALL(mock_producer, OnConnect());
  EXPECT_CALL(mock_consumer, OnConnect());
  task_runner.RunUntilIdle();

  DataSourceDescriptor ds_desc;
  ds_desc.set_name("foo");
  producer_endpoint->RegisterDataSource(ds_desc, [](DataSourceID) {});
  task_runner.RunUntilIdle();

  // Disconnecting the consumer while tracing should trigger data source
  // teardown.
  EXPECT_CALL(mock_producer, CreateDataSourceInstance(_, _));
  EXPECT_CALL(mock_producer, TearDownDataSourceInstance(_));
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("foo");
  ds_config->set_target_buffer(0);
  consumer_endpoint->EnableTracing(trace_config);
  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_consumer, OnDisconnect());
  consumer_endpoint.reset();
  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_producer, OnDisconnect());
  producer_endpoint.reset();
  Mock::VerifyAndClearExpectations(&mock_producer);
  Mock::VerifyAndClearExpectations(&mock_consumer);
}

TEST_F(ServiceImplTest, ReconnectProducerWhileTracing) {
  MockProducer mock_producer;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
      svc->ConnectProducer(&mock_producer, 123u /* uid */);
  MockConsumer mock_consumer;
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint =
      svc->ConnectConsumer(&mock_consumer);

  InSequence seq;
  EXPECT_CALL(mock_producer, OnConnect());
  EXPECT_CALL(mock_consumer, OnConnect());
  task_runner.RunUntilIdle();

  DataSourceDescriptor ds_desc;
  ds_desc.set_name("foo");
  producer_endpoint->RegisterDataSource(ds_desc, [](DataSourceID) {});
  task_runner.RunUntilIdle();

  // Disconnecting the producer while tracing should trigger data source
  // teardown.
  EXPECT_CALL(mock_producer, CreateDataSourceInstance(_, _));
  EXPECT_CALL(mock_producer, TearDownDataSourceInstance(_));
  EXPECT_CALL(mock_producer, OnDisconnect());
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("foo");
  ds_config->set_target_buffer(0);
  consumer_endpoint->EnableTracing(trace_config);
  producer_endpoint.reset();
  task_runner.RunUntilIdle();

  // Reconnecting a producer with a matching data source should see that data
  // source getting enabled.
  EXPECT_CALL(mock_producer, OnConnect());
  producer_endpoint = svc->ConnectProducer(&mock_producer, 123u /* uid */);
  task_runner.RunUntilIdle();
  EXPECT_CALL(mock_producer, CreateDataSourceInstance(_, _));
  EXPECT_CALL(mock_producer, TearDownDataSourceInstance(_));
  producer_endpoint->RegisterDataSource(ds_desc, [](DataSourceID) {});
  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_consumer, OnDisconnect());
  consumer_endpoint->DisableTracing();
  consumer_endpoint.reset();
  task_runner.RunUntilIdle();

  EXPECT_CALL(mock_producer, OnDisconnect());
  producer_endpoint.reset();
  Mock::VerifyAndClearExpectations(&mock_producer);
  Mock::VerifyAndClearExpectations(&mock_consumer);
}

TEST_F(ServiceImplTest, ProducerIDWrapping) {
  base::TestTaskRunner task_runner;
  auto shm_factory =
      std::unique_ptr<SharedMemory::Factory>(new TestSharedMemory::Factory());
  std::unique_ptr<ServiceImpl> svc(static_cast<ServiceImpl*>(
      Service::CreateInstance(std::move(shm_factory), &task_runner).release()));

  std::map<ProducerID, std::pair<std::unique_ptr<MockProducer>,
                                 std::unique_ptr<Service::ProducerEndpoint>>>
      producers;

  auto ConnectProducerAndWait = [&task_runner, &svc, &producers]() {
    char checkpoint_name[32];
    static int checkpoint_num = 0;
    sprintf(checkpoint_name, "on_connect_%d", checkpoint_num++);
    auto on_connect = task_runner.CreateCheckpoint(checkpoint_name);
    std::unique_ptr<MockProducer> producer(new MockProducer());
    std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
        svc->ConnectProducer(producer.get(), 123u /* uid */);
    EXPECT_CALL(*producer, OnConnect()).WillOnce(Invoke(on_connect));
    task_runner.RunUntilCheckpoint(checkpoint_name);
    EXPECT_EQ(&*producer_endpoint, svc->GetProducer(svc->last_producer_id_));
    const ProducerID pr_id = svc->last_producer_id_;
    producers.emplace(pr_id, std::make_pair(std::move(producer),
                                            std::move(producer_endpoint)));
    return pr_id;
  };

  auto DisconnectProducerAndWait = [&task_runner,
                                    &producers](ProducerID pr_id) {
    char checkpoint_name[32];
    static int checkpoint_num = 0;
    sprintf(checkpoint_name, "on_disconnect_%d", checkpoint_num++);
    auto on_disconnect = task_runner.CreateCheckpoint(checkpoint_name);
    auto it = producers.find(pr_id);
    PERFETTO_CHECK(it != producers.end());
    EXPECT_CALL(*it->second.first, OnDisconnect())
        .WillOnce(Invoke(on_disconnect));
    producers.erase(pr_id);
    task_runner.RunUntilCheckpoint(checkpoint_name);
  };

  // Connect producers 1-4.
  for (ProducerID i = 1; i <= 4; i++)
    ASSERT_EQ(i, ConnectProducerAndWait());

  // Disconnect producers 1,3.
  DisconnectProducerAndWait(1);
  DisconnectProducerAndWait(3);

  svc->last_producer_id_ = kMaxProducerID - 1;
  ASSERT_EQ(kMaxProducerID, ConnectProducerAndWait());
  ASSERT_EQ(1u, ConnectProducerAndWait());
  ASSERT_EQ(3u, ConnectProducerAndWait());
  ASSERT_EQ(5u, ConnectProducerAndWait());
  ASSERT_EQ(6u, ConnectProducerAndWait());

  // Disconnect all producers to mute spurious callbacks.
  DisconnectProducerAndWait(kMaxProducerID);
  for (ProducerID i = 1; i <= 6; i++)
    DisconnectProducerAndWait(i);
}

TEST_F(ServiceImplTest, WriteIntoFileAndStopOnMaxSize) {
  MockProducer mock_producer;
  std::unique_ptr<Service::ProducerEndpoint> producer_endpoint =
      svc->ConnectProducer(&mock_producer, 123u /* uid */);
  MockConsumer mock_consumer;
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint =
      svc->ConnectConsumer(&mock_consumer);

  EXPECT_CALL(mock_producer, OnConnect());
  EXPECT_CALL(mock_consumer, OnConnect());
  task_runner.RunUntilIdle();

  DataSourceDescriptor ds_desc;
  ds_desc.set_name("datasource");
  producer_endpoint->RegisterDataSource(ds_desc, [](DataSourceID) {});
  task_runner.RunUntilIdle();

  static const char kPayload[] = "1234567890abcdef-";
  static const int kNumPackets = 10;
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("datasource");
  ds_config->set_target_buffer(0);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(1);
  const uint64_t kMaxFileSize = 512;
  trace_config.set_max_file_size_bytes(kMaxFileSize);
  base::TempFile tmp_file = base::TempFile::Create();
  auto on_tracing_start = task_runner.CreateCheckpoint("on_tracing_start");
  BufferID buf_id = 0;
  EXPECT_CALL(mock_producer, OnTracingStart());
  EXPECT_CALL(mock_producer, CreateDataSourceInstance(_, _))
      .WillOnce(Invoke([on_tracing_start, &buf_id](
                           DataSourceInstanceID, const DataSourceConfig& cfg) {
        buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_tracing_start();
      }));
  consumer_endpoint->EnableTracing(trace_config,
                                   base::ScopedFile(dup(tmp_file.fd())));
  task_runner.RunUntilCheckpoint("on_tracing_start");

  std::unique_ptr<TraceWriter> writer =
      producer_endpoint->CreateTraceWriter(buf_id);
  // All these packets should fit within kMaxFileSize.
  for (int i = 0; i < kNumPackets; i++) {
    auto tp = writer->NewTracePacket();
    std::string payload(kPayload);
    payload.append(std::to_string(i));
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
  }

  // Finally add a packet that overflows kMaxFileSize. This should cause the
  // implicit stop of the trace and should *not* be written in the trace.
  {
    auto tp = writer->NewTracePacket();
    char big_payload[kMaxFileSize] = "BIG!";
    tp->set_for_testing()->set_str(big_payload, sizeof(big_payload));
  }
  writer->Flush();
  writer.reset();

  auto on_tracing_stop = task_runner.CreateCheckpoint("on_tracing_stop");
  EXPECT_CALL(mock_producer, TearDownDataSourceInstance(_));
  EXPECT_CALL(mock_consumer, OnTracingStop()).WillOnce(Invoke(on_tracing_stop));
  task_runner.RunUntilCheckpoint("on_tracing_stop");

  EXPECT_CALL(mock_consumer, OnDisconnect());
  EXPECT_CALL(mock_producer, OnDisconnect());
  consumer_endpoint->DisableTracing();
  consumer_endpoint.reset();
  producer_endpoint.reset();
  task_runner.RunUntilIdle();

  // Verify the contents of the file.
  std::string trace_raw;
  ASSERT_TRUE(base::ReadFile(tmp_file.path().c_str(), &trace_raw));
  protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_raw));
  ASSERT_GE(trace.packet_size(), kNumPackets);
  int num_testing_packet = 0;
  for (int i = 0; i < trace.packet_size(); i++) {
    const protos::TracePacket& tp = trace.packet(i);
    if (!tp.has_for_testing())
      continue;
    ASSERT_EQ(kPayload + std::to_string(num_testing_packet++),
              tp.for_testing().str());
  }
}  // namespace perfetto

}  // namespace perfetto
