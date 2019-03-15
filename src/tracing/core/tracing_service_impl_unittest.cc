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

#include "src/tracing/core/tracing_service_impl.h"

#include <string.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/file_utils.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/base/utils.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/core/trace_writer_impl.h"
#include "src/tracing/test/mock_consumer.h"
#include "src/tracing/test/mock_producer.h"
#include "src/tracing/test/test_shared_memory.h"

#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

using ::testing::_;
using ::testing::Contains;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Mock;
using ::testing::Not;
using ::testing::Property;
using ::testing::StrictMock;

namespace perfetto {

namespace {
constexpr size_t kDefaultShmSizeKb = TracingServiceImpl::kDefaultShmSize / 1024;
constexpr size_t kMaxShmSizeKb = TracingServiceImpl::kMaxShmSize / 1024;
}  // namespace

class TracingServiceImplTest : public testing::Test {
 public:
  using DataSourceInstanceState =
      TracingServiceImpl::DataSourceInstance::DataSourceInstanceState;

  TracingServiceImplTest() {
    auto shm_factory =
        std::unique_ptr<SharedMemory::Factory>(new TestSharedMemory::Factory());
    svc.reset(static_cast<TracingServiceImpl*>(
        TracingService::CreateInstance(std::move(shm_factory), &task_runner)
            .release()));
    svc->min_write_period_ms_ = 1;
  }

  std::unique_ptr<MockProducer> CreateMockProducer() {
    return std::unique_ptr<MockProducer>(
        new StrictMock<MockProducer>(&task_runner));
  }

  std::unique_ptr<MockConsumer> CreateMockConsumer() {
    return std::unique_ptr<MockConsumer>(
        new StrictMock<MockConsumer>(&task_runner));
  }

  ProducerID* last_producer_id() { return &svc->last_producer_id_; }

  uid_t GetProducerUid(ProducerID producer_id) {
    return svc->GetProducer(producer_id)->uid_;
  }

  TracingServiceImpl::TracingSession* tracing_session() {
    auto* session = svc->GetTracingSession(svc->last_tracing_session_id_);
    EXPECT_NE(nullptr, session);
    return session;
  }

  const std::set<BufferID>& GetAllowedTargetBuffers(ProducerID producer_id) {
    return svc->GetProducer(producer_id)->allowed_target_buffers_;
  }

  const std::map<WriterID, BufferID>& GetWriters(ProducerID producer_id) {
    return svc->GetProducer(producer_id)->writers_;
  }

  std::unique_ptr<SharedMemoryArbiterImpl> TakeShmemArbiterForProducer(
      ProducerID producer_id) {
    return std::move(svc->GetProducer(producer_id)->inproc_shmem_arbiter_);
  }

  size_t GetNumPendingFlushes() {
    return tracing_session()->pending_flushes.size();
  }

  void WaitForNextSyncMarker() {
    tracing_session()->last_snapshot_time = base::TimeMillis(0);
    static int attempt = 0;
    while (tracing_session()->last_snapshot_time == base::TimeMillis(0)) {
      auto checkpoint_name = "wait_snapshot_" + std::to_string(attempt++);
      auto timer_expired = task_runner.CreateCheckpoint(checkpoint_name);
      task_runner.PostDelayedTask([timer_expired] { timer_expired(); }, 1);
      task_runner.RunUntilCheckpoint(checkpoint_name);
    }
  }

  void WaitForTraceWritersChanged(ProducerID producer_id) {
    static int i = 0;
    auto checkpoint_name = "writers_changed_" + std::to_string(producer_id) +
                           "_" + std::to_string(i++);
    auto writers_changed = task_runner.CreateCheckpoint(checkpoint_name);
    auto writers = GetWriters(producer_id);
    std::function<void()> task;
    task = [&task, writers, writers_changed, producer_id, this]() {
      if (writers != GetWriters(producer_id)) {
        writers_changed();
        return;
      }
      task_runner.PostDelayedTask(task, 1);
    };
    task_runner.PostDelayedTask(task, 1);
    task_runner.RunUntilCheckpoint(checkpoint_name);
  }

  DataSourceInstanceState GetDataSourceInstanceState(const std::string& name) {
    for (const auto& kv : tracing_session()->data_source_instances) {
      if (kv.second.data_source_name == name)
        return kv.second.state;
    }
    PERFETTO_FATAL("Can't find data source instance with name %s",
                   name.c_str());
  }

  base::TestTaskRunner task_runner;
  std::unique_ptr<TracingServiceImpl> svc;
};

TEST_F(TracingServiceImplTest, RegisterAndUnregister) {
  std::unique_ptr<MockProducer> mock_producer_1 = CreateMockProducer();
  std::unique_ptr<MockProducer> mock_producer_2 = CreateMockProducer();

  mock_producer_1->Connect(svc.get(), "mock_producer_1", 123u /* uid */);
  mock_producer_2->Connect(svc.get(), "mock_producer_2", 456u /* uid */);

  ASSERT_EQ(2u, svc->num_producers());
  ASSERT_EQ(mock_producer_1->endpoint(), svc->GetProducer(1));
  ASSERT_EQ(mock_producer_2->endpoint(), svc->GetProducer(2));
  ASSERT_EQ(123u, GetProducerUid(1));
  ASSERT_EQ(456u, GetProducerUid(2));

  mock_producer_1->RegisterDataSource("foo");
  mock_producer_2->RegisterDataSource("bar");

  mock_producer_1->UnregisterDataSource("foo");
  mock_producer_2->UnregisterDataSource("bar");

  mock_producer_1.reset();
  ASSERT_EQ(1u, svc->num_producers());
  ASSERT_EQ(nullptr, svc->GetProducer(1));

  mock_producer_2.reset();
  ASSERT_EQ(nullptr, svc->GetProducer(2));

  ASSERT_EQ(0u, svc->num_producers());
}

TEST_F(TracingServiceImplTest, EnableAndDisableTracing) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, LockdownMode) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer_sameuid", geteuid());
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  trace_config.set_lockdown_mode(
      TraceConfig::LockdownModeOperation::LOCKDOWN_SET);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<MockProducer> producer_otheruid = CreateMockProducer();
  auto x = svc->ConnectProducer(producer_otheruid.get(), geteuid() + 1,
                                "mock_producer_ouid");
  EXPECT_CALL(*producer_otheruid, OnConnect()).Times(0);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer_otheruid.get());

  consumer->DisableTracing();
  consumer->FreeBuffers();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  trace_config.set_lockdown_mode(
      TraceConfig::LockdownModeOperation::LOCKDOWN_CLEAR);
  consumer->EnableTracing(trace_config);
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<MockProducer> producer_otheruid2 = CreateMockProducer();
  producer_otheruid->Connect(svc.get(), "mock_producer_ouid2", geteuid() + 1);

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ProducerNameFilterChange) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer_1");
  producer1->RegisterDataSource("data_source");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer_2");
  producer2->RegisterDataSource("data_source");

  std::unique_ptr<MockProducer> producer3 = CreateMockProducer();
  producer3->Connect(svc.get(), "mock_producer_3");
  producer3->RegisterDataSource("data_source");
  producer3->RegisterDataSource("unused_data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* data_source = trace_config.add_data_sources();
  data_source->mutable_config()->set_name("data_source");
  *data_source->add_producer_name_filter() = "mock_producer_1";

  // Enable tracing with only mock_producer_1 enabled;
  // the rest should not start up.
  consumer->EnableTracing(trace_config);

  producer1->WaitForTracingSetup();
  producer1->WaitForDataSourceSetup("data_source");
  producer1->WaitForDataSourceStart("data_source");

  EXPECT_CALL(*producer2, OnConnect()).Times(0);
  EXPECT_CALL(*producer3, OnConnect()).Times(0);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer2.get());
  Mock::VerifyAndClearExpectations(producer3.get());

  // Enable mock_producer_2, the third one should still
  // not get connected.
  *data_source->add_producer_name_filter() = "mock_producer_2";
  consumer->ChangeTraceConfig(trace_config);

  producer2->WaitForTracingSetup();
  producer2->WaitForDataSourceSetup("data_source");
  producer2->WaitForDataSourceStart("data_source");

  // Enable mock_producer_3 but also try to do an
  // unsupported change (adding a new data source);
  // mock_producer_3 should get enabled but not
  // for the new data source.
  *data_source->add_producer_name_filter() = "mock_producer_3";
  auto* dummy_data_source = trace_config.add_data_sources();
  dummy_data_source->mutable_config()->set_name("unused_data_source");
  *dummy_data_source->add_producer_name_filter() = "mock_producer_3";

  consumer->ChangeTraceConfig(trace_config);

  producer3->WaitForTracingSetup();
  EXPECT_CALL(*producer3, SetupDataSource(_, _)).Times(1);
  EXPECT_CALL(*producer3, StartDataSource(_, _)).Times(1);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer3.get());

  consumer->DisableTracing();
  consumer->FreeBuffers();
  producer1->WaitForDataSourceStop("data_source");
  producer2->WaitForDataSourceStop("data_source");

  EXPECT_CALL(*producer3, StopDataSource(_)).Times(1);

  consumer->WaitForTracingDisabled();

  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer3.get());
}

TEST_F(TracingServiceImplTest, DisconnectConsumerWhileTracing) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Disconnecting the consumer while tracing should trigger data source
  // teardown.
  consumer.reset();
  producer->WaitForDataSourceStop("data_source");
}

TEST_F(TracingServiceImplTest, ReconnectProducerWhileTracing) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Disconnecting and reconnecting a producer with a matching data source.
  // The Producer should see that data source getting enabled again.
  producer.reset();
  producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer_2");
  producer->RegisterDataSource("data_source");
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");
}

TEST_F(TracingServiceImplTest, ProducerIDWrapping) {
  std::vector<std::unique_ptr<MockProducer>> producers;
  producers.push_back(nullptr);

  auto connect_producer_and_get_id = [&producers,
                                      this](const std::string& name) {
    producers.emplace_back(CreateMockProducer());
    producers.back()->Connect(svc.get(), "mock_producer_" + name);
    return *last_producer_id();
  };

  // Connect producers 1-4.
  for (ProducerID i = 1; i <= 4; i++)
    ASSERT_EQ(i, connect_producer_and_get_id(std::to_string(i)));

  // Disconnect producers 1,3.
  producers[1].reset();
  producers[3].reset();

  *last_producer_id() = kMaxProducerID - 1;
  ASSERT_EQ(kMaxProducerID, connect_producer_and_get_id("maxid"));
  ASSERT_EQ(1u, connect_producer_and_get_id("1_again"));
  ASSERT_EQ(3u, connect_producer_and_get_id("3_again"));
  ASSERT_EQ(5u, connect_producer_and_get_id("5"));
  ASSERT_EQ(6u, connect_producer_and_get_id("6"));
}

// Note: file_write_period_ms is set to a large enough to have exactly one flush
// of the tracing buffers (and therefore at most one synchronization section),
// unless the test runs unrealistically slowly, or the implementation of the
// tracing snapshot packets changes.
TEST_F(TracingServiceImplTest, WriteIntoFileAndStopOnMaxSize) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(100000);  // 100s
  const uint64_t kMaxFileSize = 1024;
  trace_config.set_max_file_size_bytes(kMaxFileSize);
  base::TempFile tmp_file = base::TempFile::Create();
  consumer->EnableTracing(trace_config, base::ScopedFile(dup(tmp_file.fd())));

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // The preamble packets are:
  // Config
  // SystemInfo
  // 3x unknown
  static const int kNumPreamblePackets = 5;
  static const int kNumTestPackets = 10;
  static const char kPayload[] = "1234567890abcdef-";

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  // Tracing service will emit a preamble of packets (a synchronization section,
  // followed by a tracing config packet). The preamble and these test packets
  // should fit within kMaxFileSize.
  for (int i = 0; i < kNumTestPackets; i++) {
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

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  // Verify the contents of the file.
  std::string trace_raw;
  ASSERT_TRUE(base::ReadFile(tmp_file.path().c_str(), &trace_raw));
  protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_raw));

  ASSERT_EQ(trace.packet_size(), kNumPreamblePackets + kNumTestPackets);
  for (int i = 0; i < kNumTestPackets; i++) {
    const protos::TracePacket& tp = trace.packet(kNumPreamblePackets + i);
    ASSERT_EQ(kPayload + std::to_string(i++), tp.for_testing().str());
  }
}

// Test the logic that allows the trace config to set the shm total size and
// page size from the trace config. Also check that, if the config doesn't
// specify a value we fall back on the hint provided by the producer.
TEST_F(TracingServiceImplTest, ProducerShmAndPageSizeOverriddenByTraceConfig) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  const size_t kConfigPageSizesKb[] = /****/ {16, 16, 4, 0, 16, 8, 3, 4096, 4};
  const size_t kExpectedPageSizesKb[] = /**/ {16, 16, 4, 4, 16, 8, 4, 64, 4};

  const size_t kConfigSizesKb[] = /**/ {0, 16, 0, 20, 32, 7, 0, 96, 4096000};
  const size_t kHintSizesKb[] = /****/ {0, 0, 16, 32, 16, 0, 7, 96, 4096000};
  const size_t kExpectedSizesKb[] = {
      kDefaultShmSizeKb,  // Both hint and config are 0, use default.
      16,                 // Hint is 0, use config.
      16,                 // Config is 0, use hint.
      20,                 // Hint is takes precedence over the config.
      32,                 // Ditto, even if config is higher than hint.
      kDefaultShmSizeKb,  // Config is invalid and hint is 0, use default.
      kDefaultShmSizeKb,  // Config is 0 and hint is invalid, use default.
      kDefaultShmSizeKb,  // 96 KB isn't a multiple of the page size (64 KB).
      kMaxShmSizeKb       // Too big, cap at kMaxShmSize.
  };

  const size_t kNumProducers = base::ArraySize(kHintSizesKb);
  std::unique_ptr<MockProducer> producer[kNumProducers];
  for (size_t i = 0; i < kNumProducers; i++) {
    auto name = "mock_producer_" + std::to_string(i);
    producer[i] = CreateMockProducer();
    producer[i]->Connect(svc.get(), name, geteuid(), kHintSizesKb[i] * 1024);
    producer[i]->RegisterDataSource("data_source");
  }

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  for (size_t i = 0; i < kNumProducers; i++) {
    auto* producer_config = trace_config.add_producers();
    producer_config->set_producer_name("mock_producer_" + std::to_string(i));
    producer_config->set_shm_size_kb(static_cast<uint32_t>(kConfigSizesKb[i]));
    producer_config->set_page_size_kb(
        static_cast<uint32_t>(kConfigPageSizesKb[i]));
  }

  consumer->EnableTracing(trace_config);
  size_t actual_shm_sizes_kb[kNumProducers]{};
  size_t actual_page_sizes_kb[kNumProducers]{};
  for (size_t i = 0; i < kNumProducers; i++) {
    producer[i]->WaitForTracingSetup();
    producer[i]->WaitForDataSourceSetup("data_source");
    actual_shm_sizes_kb[i] =
        producer[i]->endpoint()->shared_memory()->size() / 1024;
    actual_page_sizes_kb[i] =
        producer[i]->endpoint()->shared_buffer_page_size_kb();
  }
  for (size_t i = 0; i < kNumProducers; i++) {
    producer[i]->WaitForDataSourceStart("data_source");
  }
  ASSERT_THAT(actual_page_sizes_kb, ElementsAreArray(kExpectedPageSizesKb));
  ASSERT_THAT(actual_shm_sizes_kb, ElementsAreArray(kExpectedSizesKb));
}

TEST_F(TracingServiceImplTest, ExplicitFlush) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  auto flush_request = consumer->Flush();
  producer->WaitForFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(
      consumer->ReadBuffers(),
      Contains(Property(&protos::TracePacket::for_testing,
                        Property(&protos::TestEvent::str, Eq("payload")))));
}

TEST_F(TracingServiceImplTest, ImplicitFlushOnTimedTraces) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  trace_config.set_duration_ms(1);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  producer->WaitForFlush(writer.get());

  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  EXPECT_THAT(
      consumer->ReadBuffers(),
      Contains(Property(&protos::TracePacket::for_testing,
                        Property(&protos::TestEvent::str, Eq("payload")))));
}

// Tests the monotonic semantic of flush request IDs, i.e., once a producer
// acks flush request N, all flush requests <= N are considered successful and
// acked to the consumer.
TEST_F(TracingServiceImplTest, BatchFlushes) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  auto flush_req_1 = consumer->Flush();
  auto flush_req_2 = consumer->Flush();
  auto flush_req_3 = consumer->Flush();

  // We'll deliberately let the 4th flush request timeout. Use a lower timeout
  // to keep test time short.
  auto flush_req_4 = consumer->Flush(/*timeout_ms=*/10);
  ASSERT_EQ(4u, GetNumPendingFlushes());

  // Make the producer reply only to the 3rd flush request.
  testing::InSequence seq;
  producer->WaitForFlush(nullptr, /*reply=*/false);  // Do NOT reply to flush 1.
  producer->WaitForFlush(nullptr, /*reply=*/false);  // Do NOT reply to flush 2.
  producer->WaitForFlush(writer.get());              // Reply only to flush 3.
  producer->WaitForFlush(nullptr, /*reply=*/false);  // Do NOT reply to flush 4.

  // Even if the producer explicily replied only to flush ID == 3, all the
  // previous flushed < 3 should be implicitly acked.
  ASSERT_TRUE(flush_req_1.WaitForReply());
  ASSERT_TRUE(flush_req_2.WaitForReply());
  ASSERT_TRUE(flush_req_3.WaitForReply());

  // At this point flush id == 4 should still be pending and should fail because
  // of reaching its timeout.
  ASSERT_FALSE(flush_req_4.WaitForReply());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(
      consumer->ReadBuffers(),
      Contains(Property(&protos::TracePacket::for_testing,
                        Property(&protos::TestEvent::str, Eq("payload")))));
}

TEST_F(TracingServiceImplTest, PeriodicFlush) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.set_flush_period_ms(1);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");

  const int kNumFlushes = 3;
  auto checkpoint = task_runner.CreateCheckpoint("all_flushes_done");
  int flushes_seen = 0;
  EXPECT_CALL(*producer, Flush(_, _, _))
      .WillRepeatedly(Invoke([&producer, &writer, &flushes_seen, checkpoint](
                                 FlushRequestID flush_req_id,
                                 const DataSourceInstanceID*, size_t) {
        {
          auto tp = writer->NewTracePacket();
          char payload[32];
          sprintf(payload, "f_%d", flushes_seen);
          tp->set_for_testing()->set_str(payload);
        }
        writer->Flush();
        producer->endpoint()->NotifyFlushComplete(flush_req_id);
        if (++flushes_seen == kNumFlushes)
          checkpoint();
      }));
  task_runner.RunUntilCheckpoint("all_flushes_done");

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  auto trace_packets = consumer->ReadBuffers();
  for (int i = 0; i < kNumFlushes; i++) {
    EXPECT_THAT(trace_packets,
                Contains(Property(&protos::TracePacket::for_testing,
                                  Property(&protos::TestEvent::str,
                                           Eq("f_" + std::to_string(i))))));
  }
}

// Creates a tracing session where some of the data sources set the
// |will_notify_on_stop| flag and checks that the OnTracingDisabled notification
// to the consumer is delayed until the acks are received.
TEST_F(TracingServiceImplTest, OnTracingDisabledWaitsForDataSourceStopAcks) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds_will_ack_1", /*ack_stop=*/true,
                               /*ack_start=*/true);
  producer->RegisterDataSource("ds_wont_ack");
  producer->RegisterDataSource("ds_will_ack_2", /*ack_stop=*/true,
                               /*ack_start=*/false);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_will_ack_1");
  trace_config.add_data_sources()->mutable_config()->set_name("ds_wont_ack");
  trace_config.add_data_sources()->mutable_config()->set_name("ds_will_ack_2");
  trace_config.set_duration_ms(1);
  trace_config.set_deferred_start(true);

  consumer->EnableTracing(trace_config);

  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_1"),
            DataSourceInstanceState::CONFIGURED);
  EXPECT_EQ(GetDataSourceInstanceState("ds_wont_ack"),
            DataSourceInstanceState::CONFIGURED);
  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_2"),
            DataSourceInstanceState::CONFIGURED);

  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_will_ack_1");
  producer->WaitForDataSourceSetup("ds_wont_ack");
  producer->WaitForDataSourceSetup("ds_will_ack_2");

  DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds_will_ack_1");
  DataSourceInstanceID id2 = producer->GetDataSourceInstanceId("ds_will_ack_2");

  consumer->StartTracing();

  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_1"),
            DataSourceInstanceState::STARTING);
  EXPECT_EQ(GetDataSourceInstanceState("ds_wont_ack"),
            DataSourceInstanceState::STARTED);
  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_2"),
            DataSourceInstanceState::STARTED);

  producer->WaitForDataSourceStart("ds_will_ack_1");
  producer->WaitForDataSourceStart("ds_wont_ack");
  producer->WaitForDataSourceStart("ds_will_ack_2");

  producer->endpoint()->NotifyDataSourceStarted(id1);

  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_1"),
            DataSourceInstanceState::STARTED);

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("ds_wont_ack");
  producer->WaitForFlush(writer.get());

  producer->WaitForDataSourceStop("ds_will_ack_1");
  producer->WaitForDataSourceStop("ds_wont_ack");
  producer->WaitForDataSourceStop("ds_will_ack_2");

  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_1"),
            DataSourceInstanceState::STOPPING);
  EXPECT_EQ(GetDataSourceInstanceState("ds_wont_ack"),
            DataSourceInstanceState::STOPPED);
  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_2"),
            DataSourceInstanceState::STOPPING);

  producer->endpoint()->NotifyDataSourceStopped(id1);
  producer->endpoint()->NotifyDataSourceStopped(id2);

  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_1"),
            DataSourceInstanceState::STOPPED);
  EXPECT_EQ(GetDataSourceInstanceState("ds_will_ack_2"),
            DataSourceInstanceState::STOPPED);

  // Wait for at most half of the service timeout, so that this test fails if
  // the service falls back on calling the OnTracingDisabled() because some of
  // the expected acks weren't received.
  consumer->WaitForTracingDisabled(
      TracingServiceImpl::kDataSourceStopTimeoutMs / 2);
}

// Creates a tracing session where a second data source
// is added while the service is waiting for DisableTracing
// acks; the service should not enable the new datasource
// and should not hit any asserts when the consumer is
// subsequently destroyed.
TEST_F(TracingServiceImplTest, OnDataSourceAddedWhilePendingDisableAcks) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds_will_ack", /*ack_stop=*/true);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_will_ack");
  trace_config.add_data_sources()->mutable_config()->set_name("ds_wont_ack");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  consumer->DisableTracing();

  producer->RegisterDataSource("ds_wont_ack");

  consumer.reset();
}

// Similar to OnTracingDisabledWaitsForDataSourceStopAcks, but deliberately
// skips the ack and checks that the service invokes the OnTracingDisabled()
// after the timeout.
TEST_F(TracingServiceImplTest, OnTracingDisabledCalledAnywaysInCaseOfTimeout) {
  svc->override_data_source_test_timeout_ms_for_testing = 1;
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source", /*ack_stop=*/true);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("data_source");
  trace_config.set_duration_ms(1);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  producer->WaitForFlush(writer.get());

  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

// Tests the session_id logic. Two data sources in the same tracing session
// should see the same session id.
TEST_F(TracingServiceImplTest, SessionId) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer1");
  producer1->RegisterDataSource("ds_1A");
  producer1->RegisterDataSource("ds_1B");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer2");
  producer2->RegisterDataSource("ds_2A");

  testing::InSequence seq;
  TracingSessionID last_session_id = 0;
  for (int i = 0; i < 3; i++) {
    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(128);
    trace_config.add_data_sources()->mutable_config()->set_name("ds_1A");
    trace_config.add_data_sources()->mutable_config()->set_name("ds_1B");
    trace_config.add_data_sources()->mutable_config()->set_name("ds_2A");
    trace_config.set_duration_ms(1);

    consumer->EnableTracing(trace_config);

    if (i == 0)
      producer1->WaitForTracingSetup();

    producer1->WaitForDataSourceSetup("ds_1A");
    producer1->WaitForDataSourceSetup("ds_1B");
    if (i == 0)
      producer2->WaitForTracingSetup();
    producer2->WaitForDataSourceSetup("ds_2A");

    producer1->WaitForDataSourceStart("ds_1A");
    producer1->WaitForDataSourceStart("ds_1B");
    producer2->WaitForDataSourceStart("ds_2A");

    auto* ds1 = producer1->GetDataSourceInstance("ds_1A");
    auto* ds2 = producer1->GetDataSourceInstance("ds_1B");
    auto* ds3 = producer2->GetDataSourceInstance("ds_2A");
    ASSERT_EQ(ds1->session_id, ds2->session_id);
    ASSERT_EQ(ds1->session_id, ds3->session_id);
    ASSERT_NE(ds1->session_id, last_session_id);
    last_session_id = ds1->session_id;

    auto writer1 = producer1->CreateTraceWriter("ds_1A");
    producer1->WaitForFlush(writer1.get());

    auto writer2 = producer2->CreateTraceWriter("ds_2A");
    producer2->WaitForFlush(writer2.get());

    producer1->WaitForDataSourceStop("ds_1A");
    producer1->WaitForDataSourceStop("ds_1B");
    producer2->WaitForDataSourceStop("ds_2A");
    consumer->WaitForTracingDisabled();
    consumer->FreeBuffers();
  }
}

// Writes a long trace and then tests that the trace parsed in partitions
// derived by the synchronization markers is identical to the whole trace parsed
// in one go.
TEST_F(TracingServiceImplTest, ResynchronizeTraceStreamUsingSyncMarker) {
  // Setup tracing.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(1);
  base::TempFile tmp_file = base::TempFile::Create();
  consumer->EnableTracing(trace_config, base::ScopedFile(dup(tmp_file.fd())));
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Write some variable length payload, waiting for sync markers every now
  // and then.
  const int kNumMarkers = 5;
  auto writer = producer->CreateTraceWriter("data_source");
  for (int i = 1; i <= 100; i++) {
    std::string payload(static_cast<size_t>(i), 'A' + (i % 25));
    writer->NewTracePacket()->set_for_testing()->set_str(payload.c_str());
    if (i % (100 / kNumMarkers) == 0) {
      writer->Flush();
      WaitForNextSyncMarker();
    }
  }
  writer->Flush();
  writer.reset();
  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  std::string trace_raw;
  ASSERT_TRUE(base::ReadFile(tmp_file.path().c_str(), &trace_raw));

  const auto kMarkerSize = sizeof(TracingServiceImpl::kSyncMarker);
  const std::string kSyncMarkerStr(
      reinterpret_cast<const char*>(TracingServiceImpl::kSyncMarker),
      kMarkerSize);

  // Read back the trace in partitions derived from the marker.
  // The trace should look like this:
  // [uid, marker] [event] [event] [uid, marker] [event] [event]
  size_t num_markers = 0;
  size_t start = 0;
  size_t end = 0;
  protos::Trace merged_trace;
  for (size_t pos = 0; pos != std::string::npos; start = end) {
    pos = trace_raw.find(kSyncMarkerStr, pos + 1);
    num_markers++;
    end = (pos == std::string::npos) ? trace_raw.size() : pos + kMarkerSize;
    int size = static_cast<int>(end - start);
    ASSERT_GT(size, 0);
    protos::Trace trace_partition;
    ASSERT_TRUE(trace_partition.ParseFromArray(trace_raw.data() + start, size));
    merged_trace.MergeFrom(trace_partition);
  }
  EXPECT_GE(num_markers, static_cast<size_t>(kNumMarkers));

  protos::Trace whole_trace;
  ASSERT_TRUE(whole_trace.ParseFromString(trace_raw));

  ASSERT_EQ(whole_trace.packet_size(), merged_trace.packet_size());
  EXPECT_EQ(whole_trace.SerializeAsString(), merged_trace.SerializeAsString());
}

// Creates a tracing session with |deferred_start| and checks that data sources
// are started only after calling StartTracing().
TEST_F(TracingServiceImplTest, DeferredStart) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  // Create two data sources but enable only one of them.
  producer->RegisterDataSource("ds_1");
  producer->RegisterDataSource("ds_2");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  trace_config.set_deferred_start(true);
  trace_config.set_duration_ms(1);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  // Make sure we don't get unexpected DataSourceStart() notifications yet.
  task_runner.RunUntilIdle();

  consumer->StartTracing();

  producer->WaitForDataSourceStart("ds_1");

  auto writer1 = producer->CreateTraceWriter("ds_1");
  producer->WaitForFlush(writer1.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ProducerUIDsAndPacketSequenceIDs) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer1", 123u /* uid */);
  producer1->RegisterDataSource("data_source");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer2", 456u /* uid */);
  producer2->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  producer1->WaitForTracingSetup();
  producer1->WaitForDataSourceSetup("data_source");
  producer2->WaitForTracingSetup();
  producer2->WaitForDataSourceSetup("data_source");
  producer1->WaitForDataSourceStart("data_source");
  producer2->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer1a =
      producer1->CreateTraceWriter("data_source");
  std::unique_ptr<TraceWriter> writer1b =
      producer1->CreateTraceWriter("data_source");
  std::unique_ptr<TraceWriter> writer2a =
      producer2->CreateTraceWriter("data_source");
  {
    auto tp = writer1a->NewTracePacket();
    tp->set_for_testing()->set_str("payload1a1");
    tp = writer1b->NewTracePacket();
    tp->set_for_testing()->set_str("payload1b1");
    tp = writer1a->NewTracePacket();
    tp->set_for_testing()->set_str("payload1a2");
    tp = writer2a->NewTracePacket();
    tp->set_for_testing()->set_str("payload2a1");
    tp = writer1b->NewTracePacket();
    tp->set_for_testing()->set_str("payload1b2");
  }

  auto flush_request = consumer->Flush();
  producer1->WaitForFlush({writer1a.get(), writer1b.get()});
  producer2->WaitForFlush(writer2a.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer1->WaitForDataSourceStop("data_source");
  producer2->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::TracePacket::for_testing,
                   Property(&protos::TestEvent::str, Eq("payload1a1"))),
          Property(&protos::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::TracePacket::trusted_packet_sequence_id, Eq(2u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::TracePacket::for_testing,
                   Property(&protos::TestEvent::str, Eq("payload1a2"))),
          Property(&protos::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::TracePacket::trusted_packet_sequence_id, Eq(2u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::TracePacket::for_testing,
                   Property(&protos::TestEvent::str, Eq("payload1b1"))),
          Property(&protos::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::TracePacket::trusted_packet_sequence_id, Eq(3u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::TracePacket::for_testing,
                   Property(&protos::TestEvent::str, Eq("payload1b2"))),
          Property(&protos::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::TracePacket::trusted_packet_sequence_id, Eq(3u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::TracePacket::for_testing,
                   Property(&protos::TestEvent::str, Eq("payload2a1"))),
          Property(&protos::TracePacket::trusted_uid, Eq(456)),
          Property(&protos::TracePacket::trusted_packet_sequence_id, Eq(4u)))));
}

TEST_F(TracingServiceImplTest, AllowedBuffers) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer1");
  ProducerID producer1_id = *last_producer_id();
  producer1->RegisterDataSource("data_source1");
  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer2");
  ProducerID producer2_id = *last_producer_id();
  producer2->RegisterDataSource("data_source2.1");
  producer2->RegisterDataSource("data_source2.2");
  producer2->RegisterDataSource("data_source2.3");

  EXPECT_EQ(std::set<BufferID>(), GetAllowedTargetBuffers(producer1_id));
  EXPECT_EQ(std::set<BufferID>(), GetAllowedTargetBuffers(producer2_id));

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config1 = trace_config.add_data_sources()->mutable_config();
  ds_config1->set_name("data_source1");
  ds_config1->set_target_buffer(0);
  auto* ds_config21 = trace_config.add_data_sources()->mutable_config();
  ds_config21->set_name("data_source2.1");
  ds_config21->set_target_buffer(1);
  auto* ds_config22 = trace_config.add_data_sources()->mutable_config();
  ds_config22->set_name("data_source2.2");
  ds_config22->set_target_buffer(2);
  auto* ds_config23 = trace_config.add_data_sources()->mutable_config();
  ds_config23->set_name("data_source2.3");
  ds_config23->set_target_buffer(2);  // same buffer as data_source2.2.
  consumer->EnableTracing(trace_config);

  ASSERT_EQ(3u, tracing_session()->num_buffers());
  std::set<BufferID> expected_buffers_producer1 = {
      tracing_session()->buffers_index[0]};
  std::set<BufferID> expected_buffers_producer2 = {
      tracing_session()->buffers_index[1], tracing_session()->buffers_index[2]};
  EXPECT_EQ(expected_buffers_producer1, GetAllowedTargetBuffers(producer1_id));
  EXPECT_EQ(expected_buffers_producer2, GetAllowedTargetBuffers(producer2_id));

  producer1->WaitForTracingSetup();
  producer1->WaitForDataSourceSetup("data_source1");

  producer2->WaitForTracingSetup();
  producer2->WaitForDataSourceSetup("data_source2.1");
  producer2->WaitForDataSourceSetup("data_source2.2");
  producer2->WaitForDataSourceSetup("data_source2.3");

  producer1->WaitForDataSourceStart("data_source1");
  producer2->WaitForDataSourceStart("data_source2.1");
  producer2->WaitForDataSourceStart("data_source2.2");
  producer2->WaitForDataSourceStart("data_source2.3");

  producer2->UnregisterDataSource("data_source2.3");
  producer2->WaitForDataSourceStop("data_source2.3");

  // Should still be allowed to write to buffers 1 (data_source2.1) and 2
  // (data_source2.2).
  EXPECT_EQ(expected_buffers_producer2, GetAllowedTargetBuffers(producer2_id));

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  consumer->DisableTracing();
  producer1->WaitForDataSourceStop("data_source1");
  producer2->WaitForDataSourceStop("data_source2.1");
  producer2->WaitForDataSourceStop("data_source2.2");
  consumer->WaitForTracingDisabled();

  consumer->FreeBuffers();
  EXPECT_EQ(std::set<BufferID>(), GetAllowedTargetBuffers(producer1_id));
  EXPECT_EQ(std::set<BufferID>(), GetAllowedTargetBuffers(producer2_id));
}

#if !PERFETTO_DCHECK_IS_ON()
TEST_F(TracingServiceImplTest, CommitToForbiddenBufferIsDiscarded) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  ProducerID producer_id = *last_producer_id();
  producer->RegisterDataSource("data_source");

  EXPECT_EQ(std::set<BufferID>(), GetAllowedTargetBuffers(producer_id));

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  consumer->EnableTracing(trace_config);

  ASSERT_EQ(2u, tracing_session()->num_buffers());
  std::set<BufferID> expected_buffers = {tracing_session()->buffers_index[0]};
  EXPECT_EQ(expected_buffers, GetAllowedTargetBuffers(producer_id));

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  // Try to write to the correct buffer.
  std::unique_ptr<TraceWriter> writer = producer->endpoint()->CreateTraceWriter(
      tracing_session()->buffers_index[0]);
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("good_payload");
  }

  auto flush_request = consumer->Flush();
  producer->WaitForFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  // Try to write to the wrong buffer.
  writer = producer->endpoint()->CreateTraceWriter(
      tracing_session()->buffers_index[1]);
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("bad_payload");
  }

  flush_request = consumer->Flush();
  producer->WaitForFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::TracePacket::for_testing,
                                         Property(&protos::TestEvent::str,
                                                  Eq("good_payload")))));
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("bad_payload"))))));

  consumer->FreeBuffers();
  EXPECT_EQ(std::set<BufferID>(), GetAllowedTargetBuffers(producer_id));
}
#endif  // !PERFETTO_DCHECK_IS_ON()

TEST_F(TracingServiceImplTest, RegisterAndUnregisterTraceWriter) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  ProducerID producer_id = *last_producer_id();
  producer->RegisterDataSource("data_source");

  EXPECT_TRUE(GetWriters(producer_id).empty());

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  // Creating the trace writer should register it with the service.
  std::unique_ptr<TraceWriter> writer = producer->endpoint()->CreateTraceWriter(
      tracing_session()->buffers_index[0]);

  WaitForTraceWritersChanged(producer_id);

  std::map<WriterID, BufferID> expected_writers;
  expected_writers[writer->writer_id()] = tracing_session()->buffers_index[0];
  EXPECT_EQ(expected_writers, GetWriters(producer_id));

  // Verify writing works.
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  auto flush_request = consumer->Flush();
  producer->WaitForFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  // Destroying the writer should unregister it.
  writer.reset();
  WaitForTraceWritersChanged(producer_id);
  EXPECT_TRUE(GetWriters(producer_id).empty());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload")))));
}

TEST_F(TracingServiceImplTest, ScrapeBuffersOnFlush) {
  svc->SetSMBScrapingEnabled(true);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  ProducerID producer_id = *last_producer_id();
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  std::unique_ptr<TraceWriter> writer = producer->endpoint()->CreateTraceWriter(
      tracing_session()->buffers_index[0]);
  WaitForTraceWritersChanged(producer_id);

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  // Flush but don't actually flush the chunk from TraceWriter.
  auto flush_request = consumer->Flush();
  producer->WaitForFlush(nullptr, /*reply=*/true);
  ASSERT_TRUE(flush_request.WaitForReply());

  // Chunk with the packets should have been scraped. The service can't know
  // whether the last packet was completed, so shouldn't read it.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload1")))));
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload2")))));
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("payload3"))))));

  // Write some more packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload4");
  writer->NewTracePacket()->set_for_testing()->set_str("payload5");

  // Don't reply to flush, causing a timeout. This should scrape again.
  flush_request = consumer->Flush(/*timeout=*/100);
  producer->WaitForFlush(nullptr, /*reply=*/false);
  ASSERT_FALSE(flush_request.WaitForReply());

  // Chunk with the packets should have been scraped again, overriding the
  // original one. Again, the last packet should be ignored and the first two
  // should not be read twice.
  packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("payload1"))))));
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("payload2"))))));
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload3")))));
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload4")))));
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("payload5"))))));

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

// Test scraping on producer disconnect.
TEST_F(TracingServiceImplTest, ScrapeBuffersOnProducerDisconnect) {
  svc->SetSMBScrapingEnabled(true);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  ProducerID producer_id = *last_producer_id();
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  std::unique_ptr<TraceWriter> writer = producer->endpoint()->CreateTraceWriter(
      tracing_session()->buffers_index[0]);
  WaitForTraceWritersChanged(producer_id);

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  // Disconnect the producer without committing the chunk. This should cause a
  // scrape of the SMB. Avoid destroying the ShmemArbiter until writer is
  // destroyed.
  auto shmem_arbiter = TakeShmemArbiterForProducer(producer_id);
  producer.reset();

  // Chunk with the packets should have been scraped. The service can't know
  // whether the last packet was completed, so shouldn't read it.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload1")))));
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload2")))));
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("payload3"))))));

  // Cleanup writer without causing a crash because the producer already went
  // away.
  static_cast<TraceWriterImpl*>(writer.get())->ResetChunkForTesting();
  writer.reset();
  shmem_arbiter.reset();

  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ScrapeBuffersOnDisable) {
  svc->SetSMBScrapingEnabled(true);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  ProducerID producer_id = *last_producer_id();
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling StartTracing() should be a noop (% a DLOG statement) because the
  // trace config didn't have the |deferred_start| flag set.
  consumer->StartTracing();

  std::unique_ptr<TraceWriter> writer = producer->endpoint()->CreateTraceWriter(
      tracing_session()->buffers_index[0]);
  WaitForTraceWritersChanged(producer_id);

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  // Chunk with the packets should have been scraped. The service can't know
  // whether the last packet was completed, so shouldn't read it.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload1")))));
  EXPECT_THAT(packets, Contains(Property(
                           &protos::TracePacket::for_testing,
                           Property(&protos::TestEvent::str, Eq("payload2")))));
  EXPECT_THAT(packets, Not(Contains(Property(&protos::TracePacket::for_testing,
                                             Property(&protos::TestEvent::str,
                                                      Eq("payload3"))))));
}

TEST_F(TracingServiceImplTest, AbortIfTraceDurationIsTooLong) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("datasource");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("datasource");
  trace_config.set_duration_ms(0x7fffffff);

  EXPECT_CALL(*producer, SetupDataSource(_, _)).Times(0);
  consumer->EnableTracing(trace_config);

  // The trace is aborted immediately, 5s here is just some slack for the thread
  // ping-pongs for slow devices.
  consumer->WaitForTracingDisabled(5000);
}

TEST_F(TracingServiceImplTest, GetTraceStats) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  consumer->GetTraceStats();
  consumer->WaitForTraceStats(false);

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  consumer->GetTraceStats();
  consumer->WaitForTraceStats(true);

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

}  // namespace perfetto
