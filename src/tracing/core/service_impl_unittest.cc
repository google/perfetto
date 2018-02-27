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
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/test/test_shared_memory.h"

namespace perfetto {
using ::testing::_;
using ::testing::InSequence;
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
};

class MockConsumer : public Consumer {
 public:
  ~MockConsumer() override {}

  // Consumer implementation.
  MOCK_METHOD0(OnConnect, void());
  MOCK_METHOD0(OnDisconnect, void());

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

}  // namespace perfetto
