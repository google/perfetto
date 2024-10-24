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

#include "src/tracing/service/tracing_service_impl.h"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/proc_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/sys_types.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/consumer.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/message_arena.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "perfetto/tracing/core/flush_flags.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "protos/perfetto/common/builtin_clock.gen.h"
#include "protos/perfetto/trace/clock_snapshot.gen.h"
#include "protos/perfetto/trace/remote_clock_sync.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/core/trace_writer_impl.h"
#include "src/tracing/test/mock_consumer.h"
#include "src/tracing/test/mock_producer.h"
#include "src/tracing/test/proxy_producer_endpoint.h"
#include "src/tracing/test/test_shared_memory.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/track_event_descriptor.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_uuid.gen.h"
#include "protos/perfetto/trace/trigger.gen.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zlib.h>
#include "src/tracing/service/zlib_compressor.h"
#endif

using ::testing::_;
using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;
using ::testing::Contains;
using ::testing::ContainsRegex;
using ::testing::DoAll;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::ExplainMatchResult;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::IsEmpty;
using ::testing::Mock;
using ::testing::Ne;
using ::testing::NiceMock;
using ::testing::Not;
using ::testing::Pointee;
using ::testing::Property;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::StrictMock;
using ::testing::StringMatchResultListener;
using ::testing::StrNe;
using ::testing::UnorderedElementsAre;

namespace perfetto {

namespace {
constexpr size_t kDefaultShmSizeKb = TracingServiceImpl::kDefaultShmSize / 1024;
constexpr size_t kDefaultShmPageSizeKb =
    TracingServiceImpl::kDefaultShmPageSize / 1024;
constexpr size_t kMaxShmSizeKb = TracingServiceImpl::kMaxShmSize / 1024;

AssertionResult HasTriggerModeInternal(
    const std::vector<protos::gen::TracePacket>& packets,
    protos::gen::TraceConfig::TriggerConfig::TriggerMode mode) {
  StringMatchResultListener matcher_result_string;
  bool contains = ExplainMatchResult(
      Contains(Property(
          &protos::gen::TracePacket::trace_config,
          Property(
              &protos::gen::TraceConfig::trigger_config,
              Property(&protos::gen::TraceConfig::TriggerConfig::trigger_mode,
                       Eq(mode))))),
      packets, &matcher_result_string);
  if (contains) {
    return AssertionSuccess();
  }
  return AssertionFailure() << matcher_result_string.str();
}

MATCHER_P(HasTriggerMode, mode, "") {
  return HasTriggerModeInternal(arg, mode);
}

MATCHER_P(LowerCase,
          m,
          "Lower case " + testing::DescribeMatcher<std::string>(m, negation)) {
  return ExplainMatchResult(m, base::ToLower(arg), result_listener);
}

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
std::string Decompress(const std::string& data) {
  uint8_t out[1024];

  z_stream stream{};
  stream.next_in = reinterpret_cast<uint8_t*>(const_cast<char*>(data.data()));
  stream.avail_in = static_cast<unsigned int>(data.size());

  EXPECT_EQ(inflateInit(&stream), Z_OK);
  std::string s;

  int ret;
  do {
    stream.next_out = out;
    stream.avail_out = sizeof(out);
    ret = inflate(&stream, Z_NO_FLUSH);
    EXPECT_NE(ret, Z_STREAM_ERROR);
    EXPECT_NE(ret, Z_NEED_DICT);
    EXPECT_NE(ret, Z_DATA_ERROR);
    EXPECT_NE(ret, Z_MEM_ERROR);
    s.append(reinterpret_cast<char*>(out), sizeof(out) - stream.avail_out);
  } while (ret != Z_STREAM_END);

  inflateEnd(&stream);
  return s;
}

std::vector<protos::gen::TracePacket> DecompressTrace(
    const std::vector<protos::gen::TracePacket> compressed) {
  std::vector<protos::gen::TracePacket> decompressed;

  for (const protos::gen::TracePacket& c : compressed) {
    if (c.compressed_packets().empty()) {
      decompressed.push_back(c);
      continue;
    }

    std::string s = Decompress(c.compressed_packets());
    protos::gen::Trace t;
    EXPECT_TRUE(t.ParseFromString(s));
    decompressed.insert(decompressed.end(), t.packet().begin(),
                        t.packet().end());
  }
  return decompressed;
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

std::vector<std::string> GetReceivedTriggers(
    const std::vector<protos::gen::TracePacket>& trace) {
  std::vector<std::string> triggers;
  for (const protos::gen::TracePacket& packet : trace) {
    if (packet.has_trigger()) {
      triggers.push_back(packet.trigger().trigger_name());
    }
  }
  return triggers;
}

class MockClock : public tracing_service::Clock {
 public:
  ~MockClock() override = default;
  MOCK_METHOD(base::TimeNanos, GetBootTimeNs, (), (override));
  MOCK_METHOD(base::TimeNanos, GetWallTimeNs, (), (override));
};

class MockRandom : public tracing_service::Random {
 public:
  ~MockRandom() override = default;
  MOCK_METHOD(double, GetValue, (), (override));
};

class TracingServiceImplTest : public testing::Test {
 public:
  TracingServiceImplTest() { InitializeSvcWithOpts({}); }

  void InitializeSvcWithOpts(TracingService::InitOpts init_opts) {
    auto shm_factory =
        std::unique_ptr<SharedMemory::Factory>(new TestSharedMemory::Factory());

    tracing_service::Dependencies deps;

    auto mock_clock = std::make_unique<NiceMock<MockClock>>();
    mock_clock_ = mock_clock.get();
    deps.clock = std::move(mock_clock);
    ON_CALL(*mock_clock_, GetBootTimeNs).WillByDefault(Invoke([&] {
      return real_clock_.GetBootTimeNs() + mock_clock_displacement_;
    }));
    ON_CALL(*mock_clock_, GetWallTimeNs).WillByDefault(Invoke([&] {
      return real_clock_.GetWallTimeNs() + mock_clock_displacement_;
    }));

    auto mock_random = std::make_unique<NiceMock<MockRandom>>();
    mock_random_ = mock_random.get();
    deps.random = std::move(mock_random);
    real_random_ = std::make_unique<tracing_service::RandomImpl>(
        real_clock_.GetWallTimeMs().count());
    ON_CALL(*mock_random_, GetValue).WillByDefault(Invoke([&] {
      return real_random_->GetValue();
    }));

    svc = std::make_unique<TracingServiceImpl>(
        std::move(shm_factory), &task_runner, std::move(deps), init_opts);
  }

  std::unique_ptr<MockProducer> CreateMockProducer() {
    return std::unique_ptr<MockProducer>(
        new StrictMock<MockProducer>(&task_runner));
  }

  std::unique_ptr<MockConsumer> CreateMockConsumer() {
    return std::unique_ptr<MockConsumer>(
        new StrictMock<MockConsumer>(&task_runner));
  }

  TracingSessionID GetLastTracingSessionId(MockConsumer* consumer) {
    TracingSessionID ret = 0;
    TracingServiceState svc_state = consumer->QueryServiceState();
    for (const auto& session : svc_state.tracing_sessions()) {
      TracingSessionID id = session.id();
      if (id > ret) {
        ret = id;
      }
    }
    return ret;
  }

  void AdvanceTimeAndRunUntilIdle(uint32_t ms) {
    mock_clock_displacement_ += base::TimeMillis(ms);
    task_runner.AdvanceTimeAndRunUntilIdle(ms);
  }

  base::TimeNanos mock_clock_displacement_{0};
  tracing_service::ClockImpl real_clock_;
  MockClock* mock_clock_;  // Owned by svc;
  std::unique_ptr<tracing_service::RandomImpl> real_random_;
  MockRandom* mock_random_;  // Owned by svc;

  base::TestTaskRunner task_runner;
  std::unique_ptr<TracingService> svc;
};

TEST_F(TracingServiceImplTest, AtMostOneConfig) {
  std::unique_ptr<MockConsumer> consumer_a = CreateMockConsumer();
  std::unique_ptr<MockConsumer> consumer_b = CreateMockConsumer();

  consumer_a->Connect(svc.get());
  consumer_b->Connect(svc.get());

  TraceConfig trace_config_a;
  trace_config_a.add_buffers()->set_size_kb(128);
  trace_config_a.set_duration_ms(0);
  trace_config_a.set_unique_session_name("foo");

  TraceConfig trace_config_b;
  trace_config_b.add_buffers()->set_size_kb(128);
  trace_config_b.set_duration_ms(0);
  trace_config_b.set_unique_session_name("foo");

  consumer_a->EnableTracing(trace_config_a);
  consumer_b->EnableTracing(trace_config_b);

  // This will stop immediately since it has the same unique session name.
  consumer_b->WaitForTracingDisabled();

  consumer_a->DisableTracing();
  consumer_a->WaitForTracingDisabled();

  EXPECT_THAT(consumer_b->ReadBuffers(), IsEmpty());
}

TEST_F(TracingServiceImplTest, CantBackToBackConfigsForWithExtraGuardrails) {
  {
    std::unique_ptr<MockConsumer> consumer_a = CreateMockConsumer();
    consumer_a->Connect(svc.get());

    TraceConfig trace_config_a;
    trace_config_a.add_buffers()->set_size_kb(128);
    trace_config_a.set_duration_ms(0);
    trace_config_a.set_enable_extra_guardrails(true);
    trace_config_a.set_unique_session_name("foo");

    consumer_a->EnableTracing(trace_config_a);
    consumer_a->DisableTracing();
    consumer_a->WaitForTracingDisabled();
    EXPECT_THAT(consumer_a->ReadBuffers(), Not(IsEmpty()));
  }

  {
    std::unique_ptr<MockConsumer> consumer_b = CreateMockConsumer();
    consumer_b->Connect(svc.get());

    TraceConfig trace_config_b;
    trace_config_b.add_buffers()->set_size_kb(128);
    trace_config_b.set_duration_ms(10000);
    trace_config_b.set_enable_extra_guardrails(true);
    trace_config_b.set_unique_session_name("foo");

    consumer_b->EnableTracing(trace_config_b);
    consumer_b->WaitForTracingDisabled(2000);
    EXPECT_THAT(consumer_b->ReadBuffers(), IsEmpty());
  }
}

TEST_F(TracingServiceImplTest, RegisterAndUnregister) {
  std::unique_ptr<MockProducer> mock_producer_1 = CreateMockProducer();
  std::unique_ptr<MockProducer> mock_producer_2 = CreateMockProducer();

  mock_producer_1->Connect(svc.get(), "mock_producer_1", 123u /* uid */);
  mock_producer_2->Connect(svc.get(), "mock_producer_2", 456u /* uid */);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TracingServiceState svc_state = consumer->QueryServiceState();
  ASSERT_EQ(svc_state.producers_size(), 2);
  EXPECT_EQ(svc_state.producers().at(0).id(), 1);
  EXPECT_EQ(svc_state.producers().at(0).uid(), 123);
  EXPECT_EQ(svc_state.producers().at(1).id(), 2);
  EXPECT_EQ(svc_state.producers().at(1).uid(), 456);

  mock_producer_1->RegisterDataSource("foo");
  mock_producer_2->RegisterDataSource("bar");

  mock_producer_1->UnregisterDataSource("foo");
  mock_producer_2->UnregisterDataSource("bar");

  mock_producer_1.reset();

  svc_state = consumer->QueryServiceState();
  ASSERT_EQ(svc_state.producers_size(), 1);
  EXPECT_EQ(svc_state.producers().at(0).id(), 2);

  mock_producer_2.reset();

  svc_state = consumer->QueryServiceState();
  ASSERT_EQ(svc_state.producers_size(), 0);
}

TEST_F(TracingServiceImplTest, EnableAndDisableTracing) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds = trace_config.add_data_sources();
  *ds->add_producer_name_regex_filter() = "mock_[p]roducer";
  auto* ds_config = ds->mutable_config();
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

// Creates a tracing session with a START_TRACING trigger and checks that data
// sources are started only after the service receives a trigger.
TEST_F(TracingServiceImplTest, StartTracingTriggerDeferredStart) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  // Make sure we don't get unexpected DataSourceStart() notifications yet.
  EXPECT_CALL(*producer, StartDataSource(_, _)).Times(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  // The trace won't start until we send the trigger. since we have a
  // START_TRACING trigger defined.
  std::vector<std::string> req;
  req.push_back("trigger_name");
  producer->endpoint()->ActivateTriggers(req);

  producer->WaitForDataSourceStart("ds_1");

  auto writer1 = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer1.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  std::vector<protos::gen::TracePacket> trace = consumer->ReadBuffers();
  EXPECT_THAT(
      trace,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::START_TRACING));
  EXPECT_THAT(GetReceivedTriggers(trace), ElementsAre("trigger_name"));
}

// Creates a tracing session with a START_TRACING trigger and checks that the
// session is cleaned up when no trigger is received after |trigger_timeout_ms|.
TEST_F(TracingServiceImplTest, StartTracingTriggerTimeOut) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(8.64e+7);

  trigger_config->set_trigger_timeout_ms(1);

  // Make sure we don't get unexpected DataSourceStart() notifications yet.
  EXPECT_CALL(*producer, StartDataSource(_, _)).Times(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  // The trace won't start until we send the trigger. since we have a
  // START_TRACING trigger defined. This is where we'd expect to have an
  // ActivateTriggers call to the producer->endpoint().

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());
}

// Regression test for b/274931668. An unkonwn trigger should not cause a trace
// that runs indefinitely.
TEST_F(TracingServiceImplTest, FailOnUnknownTrigger) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(
      static_cast<TraceConfig::TriggerConfig::TriggerMode>(
          TraceConfig::TriggerConfig::TriggerMode_MAX + 1));
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_from_the_future");
  trigger_config->set_trigger_timeout_ms(1);

  consumer->EnableTracing(trace_config);
  consumer->WaitForTracingDisabled();
}

// Creates a tracing session with a START_TRACING trigger and checks that
// the session is not started when the configured trigger producer is different
// than the producer that sent the trigger.
TEST_F(TracingServiceImplTest, StartTracingTriggerDifferentProducer) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(8.64e+7);
  trigger->set_producer_name_regex("correct_name");

  trigger_config->set_trigger_timeout_ms(1);

  // Make sure we don't get unexpected DataSourceStart() notifications yet.
  EXPECT_CALL(*producer, StartDataSource(_, _)).Times(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  // The trace won't start until we send the trigger called "trigger_name"
  // coming from a producer called "correct_name", since we have a
  // START_TRACING trigger defined. This is where we'd expect to have an
  // ActivateTriggers call to the producer->endpoint(), but we send the trigger
  // from a different producer so it is ignored.
  std::vector<std::string> req;
  req.push_back("trigger_name");
  producer->endpoint()->ActivateTriggers(req);

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());
}

// Creates a tracing session with a START_TRACING trigger and checks that the
// session is started when the trigger is received from the correct producer.
TEST_F(TracingServiceImplTest, StartTracingTriggerCorrectProducer) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger->set_producer_name_regex("mock_produc[e-r]+");

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  // Start the trace at this point with ActivateTriggers.
  std::vector<std::string> req;
  req.push_back("trigger_name");
  producer->endpoint()->ActivateTriggers(req);

  producer->WaitForDataSourceStart("ds_1");

  auto writer = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(
      consumer->ReadBuffers(),
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::START_TRACING));
}

// Creates a tracing session with a START_TRACING trigger and checks that the
// session is cleaned up even when a different trigger is received.
TEST_F(TracingServiceImplTest, StartTracingTriggerDifferentTrigger) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(8.64e+7);

  trigger_config->set_trigger_timeout_ms(1);

  // Make sure we don't get unexpected DataSourceStart() notifications yet.
  EXPECT_CALL(*producer, StartDataSource(_, _)).Times(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  // The trace won't start until we send the trigger called "trigger_name",
  // since we have a START_TRACING trigger defined. This is where we'd expect to
  // have an ActivateTriggers call to the producer->endpoint(), but we send a
  // different trigger.
  std::vector<std::string> req;
  req.push_back("not_correct_trigger");
  producer->endpoint()->ActivateTriggers(req);

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());
}

// Creates a tracing session with a START_TRACING trigger and checks that any
// trigger can start the TracingSession.
TEST_F(TracingServiceImplTest, StartTracingTriggerMultipleTriggers) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  std::vector<std::string> req;
  req.push_back("not_correct_trigger");
  req.push_back("trigger_name");
  producer->endpoint()->ActivateTriggers(req);

  producer->WaitForDataSourceStart("ds_1");

  auto writer = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(
      consumer->ReadBuffers(),
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::START_TRACING));
}

// Creates two tracing sessions with a START_TRACING trigger and checks that
// both are able to be triggered simultaneously.
TEST_F(TracingServiceImplTest, StartTracingTriggerMultipleTraces) {
  std::unique_ptr<MockConsumer> consumer_1 = CreateMockConsumer();
  consumer_1->Connect(svc.get());
  std::unique_ptr<MockConsumer> consumer_2 = CreateMockConsumer();
  consumer_2->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  // Create two data sources but each TracingSession will only enable one of
  // them.
  producer->RegisterDataSource("ds_1");
  producer->RegisterDataSource("ds_2");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  consumer_1->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  (*trace_config.mutable_data_sources())[0].mutable_config()->set_name("ds_2");
  trigger = trace_config.mutable_trigger_config()->add_triggers();
  trigger->set_name("trigger_name_2");
  trigger->set_stop_delay_ms(8.64e+7);

  consumer_2->EnableTracing(trace_config);

  producer->WaitForDataSourceSetup("ds_2");

  const DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds_1");
  const DataSourceInstanceID id2 = producer->GetDataSourceInstanceId("ds_2");

  std::vector<std::string> req;
  req.push_back("not_correct_trigger");
  req.push_back("trigger_name");
  req.push_back("trigger_name_2");
  producer->endpoint()->ActivateTriggers(req);

  // The order has to be the same as the triggers or else we're incorrectly wait
  // on the wrong checkpoint in the |task_runner|.
  producer->WaitForDataSourceStart("ds_1");
  producer->WaitForDataSourceStart("ds_2");

  auto writer1 = producer->CreateTraceWriter("ds_1");
  auto writer2 = producer->CreateTraceWriter("ds_2");

  // We can't use the standard WaitForX in the MockProducer and MockConsumer
  // because they assume only a single trace is going on. So we perform our own
  // expectations and wait at the end for the two consumers to receive
  // OnTracingDisabled.
  bool flushed_writer_1 = false;
  bool flushed_writer_2 = false;
  auto flush_correct_writer = [&](FlushRequestID flush_req_id,
                                  const DataSourceInstanceID* id, size_t,
                                  FlushFlags) {
    if (*id == id1) {
      flushed_writer_1 = true;
      writer1->Flush();
      producer->endpoint()->NotifyFlushComplete(flush_req_id);
    } else if (*id == id2) {
      flushed_writer_2 = true;
      writer2->Flush();
      producer->endpoint()->NotifyFlushComplete(flush_req_id);
    }
  };
  FlushFlags flush_flags(FlushFlags::Initiator::kTraced,
                         FlushFlags::Reason::kTraceStop);
  EXPECT_CALL(*producer, Flush(_, _, _, flush_flags))
      .WillOnce(Invoke(flush_correct_writer))
      .WillOnce(Invoke(flush_correct_writer));

  auto checkpoint_name = "on_tracing_disabled_consumer_1_and_2";
  auto on_tracing_disabled = task_runner.CreateCheckpoint(checkpoint_name);
  std::atomic<size_t> counter(0);
  EXPECT_CALL(*consumer_1, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs([&]() {
        if (++counter == 2u) {
          on_tracing_disabled();
        }
      }));
  EXPECT_CALL(*consumer_2, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs([&]() {
        if (++counter == 2u) {
          on_tracing_disabled();
        }
      }));

  EXPECT_CALL(*producer, StopDataSource(id1));
  EXPECT_CALL(*producer, StopDataSource(id2));

  task_runner.RunUntilCheckpoint(checkpoint_name, 1000);

  EXPECT_TRUE(flushed_writer_1);
  EXPECT_TRUE(flushed_writer_2);

  std::vector<protos::gen::TracePacket> trace1 = consumer_1->ReadBuffers();
  EXPECT_THAT(
      trace1,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::START_TRACING));
  EXPECT_THAT(GetReceivedTriggers(trace1), ElementsAre("trigger_name"));
  std::vector<protos::gen::TracePacket> trace2 = consumer_2->ReadBuffers();
  EXPECT_THAT(
      trace2,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::START_TRACING));
  EXPECT_THAT(GetReceivedTriggers(trace2),
              UnorderedElementsAre("trigger_name", "trigger_name_2"));
}

// Creates a tracing session with a START_TRACING trigger and checks that the
// received_triggers are emitted as packets.
TEST_F(TracingServiceImplTest, EmitTriggersWithStartTracingTrigger) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer", /* uid = */ 123u);

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::START_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger->set_producer_name_regex("mock_produc[e-r]+");

  trigger_config->set_trigger_timeout_ms(30000);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");

  // The trace won't start until we send the trigger since we have a
  // START_TRACING trigger defined.
  std::vector<std::string> req;
  req.push_back("trigger_name");
  req.push_back("trigger_name_2");
  req.push_back("trigger_name_3");
  producer->endpoint()->ActivateTriggers(req);

  producer->WaitForDataSourceStart("ds_1");
  auto writer1 = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer1.get());
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::START_TRACING));
  EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));
}

// Creates a tracing session with a STOP_TRACING trigger and checks that the
// received_triggers are emitted as packets.
TEST_F(TracingServiceImplTest, EmitTriggersWithStopTracingTrigger) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer", /* uid = */ 321u);

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(30000);

  trigger_config->set_trigger_timeout_ms(30000);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  // The trace won't start until we send the trigger since we have a
  // START_TRACING trigger defined.
  std::vector<std::string> req;
  req.push_back("trigger_name");
  req.push_back("trigger_name_2");
  req.push_back("trigger_name_3");
  producer->endpoint()->ActivateTriggers(req);

  auto writer1 = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer1.get());
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
  EXPECT_THAT(GetReceivedTriggers(packets),
              UnorderedElementsAre("trigger_name", "trigger_name_3"));
}

// Creates a tracing session with a STOP_TRACING trigger and checks that the
// received_triggers are emitted as packets even ones after the initial
// ReadBuffers() call.
TEST_F(TracingServiceImplTest, EmitTriggersRepeatedly) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name_2");
  trigger->set_stop_delay_ms(1);

  trigger_config->set_trigger_timeout_ms(30000);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  // The trace won't start until we send the trigger. since we have a
  // START_TRACING trigger defined.
  producer->endpoint()->ActivateTriggers({"trigger_name"});

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
  EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));

  // Send a new trigger.
  producer->endpoint()->ActivateTriggers({"trigger_name_2"});

  auto writer1 = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer1.get());
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  packets = consumer->ReadBuffers();
  // We don't rewrite the old trigger.
  EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name_2"));
}

// Creates a tracing session with a STOP_TRACING trigger and checks that the
// session is cleaned up after |trigger_timeout_ms|.
TEST_F(TracingServiceImplTest, StopTracingTriggerTimeout) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");

  trigger_config->set_trigger_timeout_ms(1);

  consumer->EnableTracing(trace_config);

  // The trace won't return data because there has been no trigger
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

  consumer->WaitForTracingDisabled();

  // The trace won't return data because there has been no trigger
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());
}

// Creates a tracing session with a STOP_TRACING trigger and checks that the
// session returns data after a trigger is received, but only what is currently
// in the buffer.
TEST_F(TracingServiceImplTest, StopTracingTriggerRingBuffer) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  // The trace won't return data until unless we send a trigger at this point.
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

  // We write into the buffer a large packet which takes up the whole buffer. We
  // then add a bunch of smaller ones which causes the larger packet to be
  // dropped. After we activate the session we should only see a bunch of the
  // smaller ones.
  static const size_t kNumTestPackets = 10;
  static const char kPayload[] = "1234567890abcdef-";

  auto writer = producer->CreateTraceWriter("ds_1");
  // Buffer is 1kb so we write a packet which is slightly smaller so it fits in
  // the buffer.
  const std::string large_payload(1024 * 128 - 20, 'a');
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str(large_payload.c_str(), large_payload.size());
  }

  // Now we add a bunch of data before the trigger and after.
  for (size_t i = 0; i < kNumTestPackets; i++) {
    if (i == kNumTestPackets / 2) {
      std::vector<std::string> req;
      req.push_back("trigger_name");
      producer->endpoint()->ActivateTriggers(req);
    }
    auto tp = writer->NewTracePacket();
    std::string payload(kPayload);
    payload.append(std::to_string(i));
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
  }
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));
  EXPECT_LT(kNumTestPackets, packets.size());
  // We expect for the TraceConfig preamble packet to be there correctly and
  // then we expect each payload to be there, but not the |large_payload|
  // packet.
  EXPECT_THAT(
      packets,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
  for (size_t i = 0; i < kNumTestPackets; i++) {
    std::string payload = kPayload;
    payload += std::to_string(i);
    EXPECT_THAT(packets,
                Contains(Property(
                    &protos::gen::TracePacket::for_testing,
                    Property(&protos::gen::TestEvent::str, Eq(payload)))));
  }

  // The large payload was overwritten before we trigger and ReadBuffers so it
  // should not be in the returned data.
  EXPECT_THAT(packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq(large_payload))))));
}

// Creates a tracing session with a STOP_TRACING trigger and checks that the
// session only cleans up once even with multiple triggers.
TEST_F(TracingServiceImplTest, StopTracingTriggerMultipleTriggers) {
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
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name_2");
  trigger->set_stop_delay_ms(8.64e+7);

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  // The trace won't return data until unless we send a trigger at this point.
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

  std::vector<std::string> req;
  req.push_back("trigger_name");
  req.push_back("trigger_name_3");
  req.push_back("trigger_name_2");
  producer->endpoint()->ActivateTriggers(req);

  auto writer = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
  EXPECT_THAT(GetReceivedTriggers(packets),
              UnorderedElementsAre("trigger_name", "trigger_name_2"));
}

TEST_F(TracingServiceImplTest, SecondTriggerHitsLimit) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);

  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_config->set_trigger_timeout_ms(8.64e+7);

  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger->set_max_per_24_h(1);

  auto* ds = trace_config.add_data_sources()->mutable_config();

  // First session.
  {
    std::unique_ptr<MockProducer> producer = CreateMockProducer();
    producer->Connect(svc.get(), "mock_producer_a");
    producer->RegisterDataSource("data_source_a");

    std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
    consumer->Connect(svc.get());

    ds->set_name("data_source_a");
    consumer->EnableTracing(trace_config);
    producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("data_source_a");
    producer->WaitForDataSourceStart("data_source_a");

    std::vector<std::string> req;
    req.push_back("trigger_name");
    producer->endpoint()->ActivateTriggers(req);

    auto writer = producer->CreateTraceWriter("data_source_a");
    producer->ExpectFlush(writer.get());

    producer->WaitForDataSourceStop("data_source_a");
    consumer->WaitForTracingDisabled();
    std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
    EXPECT_THAT(
        packets,
        HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
    EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));
  }

  AdvanceTimeAndRunUntilIdle(23 * 60 * 60 * 1000);  // 23h

  // Second session.
  {
    std::unique_ptr<MockProducer> producer = CreateMockProducer();
    producer->Connect(svc.get(), "mock_producer_b");
    producer->RegisterDataSource("data_source_b");

    std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
    consumer->Connect(svc.get());

    ds->set_name("data_source_b");
    consumer->EnableTracing(trace_config);
    producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("data_source_b");
    producer->WaitForDataSourceStart("data_source_b");

    std::vector<std::string> req;
    req.push_back("trigger_name");
    producer->endpoint()->ActivateTriggers(req);

    consumer->DisableTracing();

    producer->WaitForDataSourceStop("data_source_b");
    consumer->WaitForTracingDisabled();
    // When triggers are not hit, the tracing session doesn't return any data.
    EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

    consumer->FreeBuffers();
  }
}

TEST_F(TracingServiceImplTest, SecondTriggerDoesntHitLimit) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);

  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_config->set_trigger_timeout_ms(8.64e+7);

  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger->set_max_per_24_h(1);

  auto* ds = trace_config.add_data_sources()->mutable_config();

  // First session.
  {
    std::unique_ptr<MockProducer> producer = CreateMockProducer();
    producer->Connect(svc.get(), "mock_producer_a");
    producer->RegisterDataSource("data_source_a");

    std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
    consumer->Connect(svc.get());

    ds->set_name("data_source_a");
    consumer->EnableTracing(trace_config);
    producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("data_source_a");
    producer->WaitForDataSourceStart("data_source_a");

    std::vector<std::string> req;
    req.push_back("trigger_name");
    producer->endpoint()->ActivateTriggers(req);

    auto writer = producer->CreateTraceWriter("data_source_a");
    producer->ExpectFlush(writer.get());

    producer->WaitForDataSourceStop("data_source_a");
    consumer->WaitForTracingDisabled();
    std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
    EXPECT_THAT(
        packets,
        HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
    EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));
  }

  AdvanceTimeAndRunUntilIdle(24 * 60 * 60 * 1000);  // 24h

  // Second session.
  {
    std::unique_ptr<MockProducer> producer = CreateMockProducer();
    producer->Connect(svc.get(), "mock_producer_b");
    producer->RegisterDataSource("data_source_b");

    std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
    consumer->Connect(svc.get());

    ds->set_name("data_source_b");
    consumer->EnableTracing(trace_config);
    producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("data_source_b");
    producer->WaitForDataSourceStart("data_source_b");

    std::vector<std::string> req;
    req.push_back("trigger_name");
    producer->endpoint()->ActivateTriggers(req);

    auto writer = producer->CreateTraceWriter("data_source_b");
    producer->ExpectFlush(writer.get());

    producer->WaitForDataSourceStop("data_source_b");
    consumer->WaitForTracingDisabled();
    std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
    EXPECT_THAT(
        packets,
        HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
    EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));
  }
}

TEST_F(TracingServiceImplTest, SkipProbability) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("data_source");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  trigger->set_stop_delay_ms(1);
  trigger->set_skip_probability(0.15);

  trigger_config->set_trigger_timeout_ms(8.64e+7);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::vector<std::string> req;
  req.push_back("trigger_name");

  // This is below the probability of 0.15 so should be skipped.
  EXPECT_CALL(*mock_random_, GetValue).WillOnce(Return(0.14));
  producer->endpoint()->ActivateTriggers(req);

  // When triggers are not hit, the tracing session doesn't return any data.
  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

  // This is above the probability of 0.15 so should be allowed.
  EXPECT_CALL(*mock_random_, GetValue).WillOnce(Return(0.16));
  producer->endpoint()->ActivateTriggers(req);

  auto writer = producer->CreateTraceWriter("data_source");
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      HasTriggerMode(protos::gen::TraceConfig::TriggerConfig::STOP_TRACING));
  EXPECT_THAT(GetReceivedTriggers(packets), ElementsAre("trigger_name"));
}

// Creates a tracing session with a CLONE_SNAPSHOT trigger and checks that
// ReadBuffer calls on it return consistently no data (as in the case of
// STOP_TRACING with no triggers hit) to avoid double uploads (b/290799105 and
// b/290798988).
TEST_F(TracingServiceImplTest, CloneSnapshotTriggers) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::CLONE_SNAPSHOT);
  trigger_config->set_trigger_timeout_ms(8.64e+7);
  for (int i = 0; i < 3; i++) {
    auto* trigger = trigger_config->add_triggers();
    trigger->set_name("trigger_" + std::to_string(i));
    trigger->set_stop_delay_ms(1);
  }

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

  auto writer = producer->CreateTraceWriter("ds_1");

  std::optional<TracingSessionID> orig_tsid;

  // Iterate over a sequence of trigger + CloneSession, to emulate a long trace
  // receiving different triggers and being cloned several times.
  for (int iter = 0; iter < 3; iter++) {
    std::string trigger_name = "trigger_" + std::to_string(iter);
    producer->endpoint()->ActivateTriggers({trigger_name});

    // Reading the original trace session should always return nothing. Only the
    // cloned sessions should return data.
    EXPECT_THAT(consumer->ReadBuffers(), IsEmpty());

    // Now clone the session and check that the cloned session has the triggers.
    std::unique_ptr<MockConsumer> clone_cons = CreateMockConsumer();
    clone_cons->Connect(svc.get());
    if (!orig_tsid) {
      orig_tsid = GetLastTracingSessionId(clone_cons.get());
    }

    std::string checkpoint_name = "clone_done_" + std::to_string(iter);
    auto clone_done = task_runner.CreateCheckpoint(checkpoint_name);
    EXPECT_CALL(*clone_cons, OnSessionCloned(_))
        .WillOnce(InvokeWithoutArgs(clone_done));
    clone_cons->CloneSession(*orig_tsid);
    // CloneSession() will implicitly issue a flush. Linearize with that.
    producer->ExpectFlush(writer.get());
    task_runner.RunUntilCheckpoint(checkpoint_name);

    // Read the cloned session and ensure it only contains the last trigger
    // (i.e. check that the trigger history is reset after each clone and
    // doesn't pile up).
    auto packets = clone_cons->ReadBuffers();
    auto expect_received_trigger = [](const std::string& name) {
      return Contains(
          Property(&protos::gen::TracePacket::trigger,
                   Property(&protos::gen::Trigger::trigger_name, Eq(name))));
    };
    EXPECT_THAT(packets, expect_received_trigger(trigger_name));
    EXPECT_THAT(
        packets,
        Not(expect_received_trigger("trigger_" + std::to_string(iter - 1))));
  }  // for (iter)

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, LockdownMode) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer_sameuid",
                    base::GetCurrentUserId());
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  trace_config.set_lockdown_mode(TraceConfig::LOCKDOWN_SET);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<MockProducer> producer_otheruid = CreateMockProducer();
  auto x = svc->ConnectProducer(
      producer_otheruid.get(),
      ClientIdentity(base::GetCurrentUserId() + 1, base::GetProcessId()),
      "mock_producer_ouid");
  EXPECT_CALL(*producer_otheruid, OnConnect()).Times(0);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer_otheruid.get());

  consumer->DisableTracing();
  consumer->FreeBuffers();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  trace_config.set_lockdown_mode(TraceConfig::LOCKDOWN_CLEAR);
  consumer->EnableTracing(trace_config);
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<MockProducer> producer_otheruid2 = CreateMockProducer();
  producer_otheruid->Connect(svc.get(), "mock_producer_ouid2",
                             base::GetCurrentUserId() + 1);

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
  *data_source->add_producer_name_regex_filter() = ".*_producer_[2]";
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

TEST_F(TracingServiceImplTest, ProducerNameFilterChangeTwoDataSources) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer_1");
  producer1->RegisterDataSource("data_source");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer_2");
  producer2->RegisterDataSource("data_source");
  producer2->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* data_source = trace_config.add_data_sources();
  data_source->mutable_config()->set_name("data_source");
  *data_source->add_producer_name_filter() = "mock_producer_1";

  // Enable tracing with only mock_producer_1 enabled;
  // the rest should not start up.
  consumer->EnableTracing(trace_config);

  producer1->WaitForTracingSetup();
  EXPECT_CALL(*producer1, SetupDataSource(_, _)).Times(1);
  EXPECT_CALL(*producer1, StartDataSource(_, _)).Times(1);

  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer1.get());
  Mock::VerifyAndClearExpectations(producer2.get());

  // Enable mock_producer_2, both instances of "data_source" should start
  *data_source->add_producer_name_regex_filter() = ".*_producer_[2]";
  consumer->ChangeTraceConfig(trace_config);

  producer2->WaitForTracingSetup();
  EXPECT_CALL(*producer2, SetupDataSource(_, _)).Times(2);
  EXPECT_CALL(*producer2, StartDataSource(_, _)).Times(2);

  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer1.get());
  Mock::VerifyAndClearExpectations(producer2.get());

  consumer->DisableTracing();
  consumer->FreeBuffers();

  EXPECT_CALL(*producer1, StopDataSource(_)).Times(1);
  EXPECT_CALL(*producer2, StopDataSource(_)).Times(2);

  consumer->WaitForTracingDisabled();

  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(producer1.get());
  Mock::VerifyAndClearExpectations(producer2.get());
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

TEST_F(TracingServiceImplTest, CompressionConfiguredButUnsupported) {
  // Initialize the service without support for compression.
  TracingService::InitOpts init_opts;
  init_opts.compressor_fn = nullptr;
  InitializeSvcWithOpts(init_opts);

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
  // Ask for compression in the config.
  trace_config.set_compression_type(TraceConfig::COMPRESSION_TYPE_DEFLATE);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-1");
  }
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-2");
  }

  writer->Flush();
  writer.reset();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  // The packets should NOT be compressed.
  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Not(IsEmpty()));
  EXPECT_THAT(
      packets,
      Each(Property(&protos::gen::TracePacket::has_compressed_packets, false)));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload-1")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload-2")))));
}

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
TEST_F(TracingServiceImplTest, CompressionReadIpc) {
  TracingService::InitOpts init_opts;
  init_opts.compressor_fn = ZlibCompressFn;
  InitializeSvcWithOpts(init_opts);

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
  trace_config.set_compression_type(TraceConfig::COMPRESSION_TYPE_DEFLATE);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-1");
  }
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-2");
  }

  writer->Flush();
  writer.reset();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  std::vector<protos::gen::TracePacket> compressed_packets =
      consumer->ReadBuffers();
  EXPECT_THAT(compressed_packets, Not(IsEmpty()));
  EXPECT_THAT(compressed_packets,
              Each(Property(&protos::gen::TracePacket::compressed_packets,
                            Not(IsEmpty()))));
  std::vector<protos::gen::TracePacket> decompressed_packets =
      DecompressTrace(compressed_packets);
  EXPECT_THAT(decompressed_packets,
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload-1")))));
  EXPECT_THAT(decompressed_packets,
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload-2")))));
}

TEST_F(TracingServiceImplTest, CompressionWriteIntoFile) {
  TracingService::InitOpts init_opts;
  init_opts.compressor_fn = ZlibCompressFn;
  InitializeSvcWithOpts(init_opts);

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
  trace_config.set_compression_type(TraceConfig::COMPRESSION_TYPE_DEFLATE);
  base::TempFile tmp_file = base::TempFile::Create();
  consumer->EnableTracing(trace_config, base::ScopedFile(dup(tmp_file.fd())));

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-1");
  }
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-2");
  }

  writer->Flush();
  writer.reset();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  // Verify the contents of the file.
  std::string trace_raw;
  ASSERT_TRUE(base::ReadFile(tmp_file.path().c_str(), &trace_raw));
  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_raw));
  EXPECT_THAT(trace.packet(), Not(IsEmpty()));
  EXPECT_THAT(trace.packet(),
              Each(Property(&protos::gen::TracePacket::compressed_packets,
                            Not(IsEmpty()))));
  std::vector<protos::gen::TracePacket> decompressed_packets =
      DecompressTrace(trace.packet());
  EXPECT_THAT(decompressed_packets,
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload-1")))));
  EXPECT_THAT(decompressed_packets,
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload-2")))));
}

TEST_F(TracingServiceImplTest, CloneSessionWithCompression) {
  TracingService::InitOpts init_opts;
  init_opts.compressor_fn = ZlibCompressFn;
  InitializeSvcWithOpts(init_opts);

  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  // The consumer that clones it and reads back the data.
  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  trace_config.set_compression_type(TraceConfig::COMPRESSION_TYPE_DEFLATE);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");

  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");

  // Add some data.
  static constexpr size_t kNumTestPackets = 20;
  for (size_t i = 0; i < kNumTestPackets; i++) {
    auto tp = writer->NewTracePacket();
    std::string payload("payload" + std::to_string(i));
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
    tp->set_timestamp(static_cast<uint64_t>(i));
  }

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  EXPECT_CALL(*consumer2, OnSessionCloned(_))
      .WillOnce(Invoke([clone_done](const Consumer::OnSessionClonedArgs&) {
        clone_done();
      }));
  consumer2->CloneSession(1);
  // CloneSession() will implicitly issue a flush. Linearize with that.
  FlushFlags expected_flags(FlushFlags::Initiator::kTraced,
                            FlushFlags::Reason::kTraceClone);
  producer->ExpectFlush(writer.get(), /*reply=*/true, expected_flags);
  task_runner.RunUntilCheckpoint("clone_done");

  // Delete the initial tracing session.
  consumer->DisableTracing();
  consumer->FreeBuffers();
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  // Read back the cloned trace and check that it's compressed
  std::vector<protos::gen::TracePacket> compressed_packets =
      consumer2->ReadBuffers();
  EXPECT_THAT(compressed_packets, Not(IsEmpty()));
  EXPECT_THAT(compressed_packets,
              Each(Property(&protos::gen::TracePacket::compressed_packets,
                            Not(IsEmpty()))));
}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

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
  // Trace start clock snapshot
  // Trace most recent clock snapshot
  // Trace synchronisation
  // TraceUuid
  // Config
  // SystemInfo
  // Tracing started (TracingServiceEvent)
  // All data source started (TracingServiceEvent)
  // Tracing disabled (TracingServiceEvent)
  static const int kNumPreamblePackets = 9;
  static const int kNumTestPackets = 9;
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
  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_raw));

  ASSERT_EQ(trace.packet_size(), kNumPreamblePackets + kNumTestPackets);
  for (size_t i = 0; i < kNumTestPackets; i++) {
    const protos::gen::TracePacket& tp =
        trace.packet()[kNumPreamblePackets + i];
    ASSERT_EQ(kPayload + std::to_string(i++), tp.for_testing().str());
  }
}

TEST_F(TracingServiceImplTest, WriteIntoFileWithPath) {
  auto tmp_file = base::TempFile::Create();
  // Deletes the file (the service would refuse to overwrite an existing file)
  // without telling it to the underlying TempFile, so that its dtor will
  // unlink the file created by the service.
  unlink(tmp_file.path().c_str());

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
  trace_config.set_output_path(tmp_file.path());
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
  writer->Flush();
  writer.reset();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  // Verify the contents of the file.
  std::string trace_raw;
  ASSERT_TRUE(base::ReadFile(tmp_file.path(), &trace_raw));
  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_raw));
  // ASSERT_EQ(trace.packet_size(), 33);
  EXPECT_THAT(trace.packet(),
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload")))));
}

TEST_F(TracingServiceImplTest, WriteIntoFileFilterMultipleChunks) {
  static const size_t kNumTestPackets = 5;
  static const size_t kPayloadSize = 500 * 1024UL;
  static_assert(kNumTestPackets * kPayloadSize >
                    TracingServiceImpl::kWriteIntoFileChunkSize,
                "This test covers filtering multiple chunks");

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

  protozero::FilterBytecodeGenerator filt;
  // Message 0: root Trace proto.
  filt.AddNestedField(1 /* root trace.packet*/, 1);
  filt.EndMessage();
  // Message 1: TracePacket proto. Allow all fields.
  filt.AddSimpleFieldRange(1, 1000);
  filt.EndMessage();
  trace_config.mutable_trace_filter()->set_bytecode(filt.Serialize());

  base::TempFile tmp_file = base::TempFile::Create();
  consumer->EnableTracing(trace_config, base::ScopedFile(dup(tmp_file.fd())));

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  for (size_t i = 0; i < kNumTestPackets; i++) {
    auto tp = writer->NewTracePacket();
    std::string payload(kPayloadSize, 'c');
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
  }

  writer->Flush();
  writer.reset();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  consumer->GetTraceStats();
  TraceStats stats = consumer->WaitForTraceStats(true);

  std::string trace_raw;
  ASSERT_TRUE(base::ReadFile(tmp_file.path().c_str(), &trace_raw));
  protozero::ProtoDecoder dec(trace_raw.data(), trace_raw.size());
  size_t total_size = 0;
  for (auto field = dec.ReadField(); field.valid(); field = dec.ReadField()) {
    total_size += field.size();
  }
  EXPECT_EQ(total_size, stats.filter_stats().output_bytes());
  EXPECT_GT(total_size, kNumTestPackets * kPayloadSize);
}

// Test the logic that allows the trace config to set the shm total size and
// page size from the trace config. Also check that, if the config doesn't
// specify a value we fall back on the hint provided by the producer.
TEST_F(TracingServiceImplTest, ProducerShmAndPageSizeOverriddenByTraceConfig) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  const size_t kMaxPageSizeKb = 32;

  struct ConfiguredAndExpectedSizes {
    size_t config_page_size_kb;
    size_t hint_page_size_kb;
    size_t expected_page_size_kb;

    size_t config_size_kb;
    size_t hint_size_kb;
    size_t expected_size_kb;
  };

  ConfiguredAndExpectedSizes kSizes[] = {
      // Config and hint are 0, fallback to default values.
      {0, 0, kDefaultShmPageSizeKb, 0, 0, kDefaultShmSizeKb},
      // Use configured sizes.
      {16, 0, 16, 16, 0, 16},
      // Config is 0, use hint.
      {0, 4, 4, 0, 16, 16},
      // Config takes precendence over hint.
      {4, 8, 4, 16, 32, 16},
      // Config takes precendence over hint, even if it's larger.
      {8, 4, 8, 32, 16, 32},
      // Config page size % 4 != 0, fallback to defaults.
      {3, 0, kDefaultShmPageSizeKb, 0, 0, kDefaultShmSizeKb},
      // Config page size less than system page size, fallback to defaults.
      {2, 0, kDefaultShmPageSizeKb, 0, 0, kDefaultShmSizeKb},
      // Config sizes too large, use max.
      {4096, 0, kMaxPageSizeKb, 4096000, 0, kMaxShmSizeKb},
      // Hint sizes too large, use max.
      {0, 4096, kMaxPageSizeKb, 0, 4096000, kMaxShmSizeKb},
      // Config buffer size isn't a multiple of 4KB, fallback to defaults.
      {0, 0, kDefaultShmPageSizeKb, 18, 0, kDefaultShmSizeKb},
      // Invalid page size -> also ignore buffer size config.
      {2, 0, kDefaultShmPageSizeKb, 32, 0, kDefaultShmSizeKb},
      // Invalid buffer size -> also ignore page size config.
      {16, 0, kDefaultShmPageSizeKb, 18, 0, kDefaultShmSizeKb},
      // Config page size % buffer size != 0, fallback to defaults.
      {8, 0, kDefaultShmPageSizeKb, 20, 0, kDefaultShmSizeKb},
      // Config page size % default buffer size != 0, fallback to defaults.
      {28, 0, kDefaultShmPageSizeKb, 0, 0, kDefaultShmSizeKb},
  };

  const size_t kNumProducers = base::ArraySize(kSizes);
  std::unique_ptr<MockProducer> producer[kNumProducers];
  for (size_t i = 0; i < kNumProducers; i++) {
    auto name = "mock_producer_" + std::to_string(i);
    producer[i] = CreateMockProducer();
    producer[i]->Connect(svc.get(), name, base::GetCurrentUserId(),
                         base::GetProcessId(), kSizes[i].hint_size_kb * 1024,
                         kSizes[i].hint_page_size_kb * 1024);
    producer[i]->RegisterDataSource("data_source");
  }

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  for (size_t i = 0; i < kNumProducers; i++) {
    auto* producer_config = trace_config.add_producers();
    producer_config->set_producer_name("mock_producer_" + std::to_string(i));
    producer_config->set_shm_size_kb(
        static_cast<uint32_t>(kSizes[i].config_size_kb));
    producer_config->set_page_size_kb(
        static_cast<uint32_t>(kSizes[i].config_page_size_kb));
  }

  consumer->EnableTracing(trace_config);
  size_t expected_shm_sizes_kb[kNumProducers]{};
  size_t expected_page_sizes_kb[kNumProducers]{};
  size_t actual_shm_sizes_kb[kNumProducers]{};
  size_t actual_page_sizes_kb[kNumProducers]{};
  for (size_t i = 0; i < kNumProducers; i++) {
    expected_shm_sizes_kb[i] = kSizes[i].expected_size_kb;
    expected_page_sizes_kb[i] = kSizes[i].expected_page_size_kb;

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
  ASSERT_THAT(actual_page_sizes_kb, ElementsAreArray(expected_page_sizes_kb));
  ASSERT_THAT(actual_shm_sizes_kb, ElementsAreArray(expected_shm_sizes_kb));
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
  FlushFlags expected_flags(FlushFlags::Initiator::kConsumerSdk,
                            FlushFlags::Reason::kExplicit);
  producer->ExpectFlush(writer.get(), /*reply=*/true, expected_flags);
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(consumer->ReadBuffers(),
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload")))));
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

  FlushFlags expected_flags(FlushFlags::Initiator::kTraced,
                            FlushFlags::Reason::kTraceStop);
  producer->ExpectFlush(writer.get(), /*reply=*/true, expected_flags);

  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  EXPECT_THAT(consumer->ReadBuffers(),
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload")))));
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

  FlushRequestID third_flush_id;
  auto checkpoint = task_runner.CreateCheckpoint("all_flushes_received");
  EXPECT_CALL(*producer, Flush)
      .WillOnce(Return())
      .WillOnce(Return())
      .WillOnce(SaveArg<0>(&third_flush_id))
      .WillOnce(InvokeWithoutArgs([checkpoint] { checkpoint(); }));

  auto flush_req_1 = consumer->Flush();
  auto flush_req_2 = consumer->Flush();
  auto flush_req_3 = consumer->Flush();

  // We'll deliberately let the 4th flush request timeout. Use a lower timeout
  // to keep test time short.
  auto flush_req_4 = consumer->Flush(/*timeout_ms=*/10);

  task_runner.RunUntilCheckpoint("all_flushes_received");

  writer->Flush();
  // Reply only to flush 3. Do not reply to 1,2 and 4.
  producer->endpoint()->NotifyFlushComplete(third_flush_id);

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
  EXPECT_THAT(consumer->ReadBuffers(),
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload")))));
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
  FlushFlags flush_flags(FlushFlags::Initiator::kTraced,
                         FlushFlags::Reason::kPeriodic);
  EXPECT_CALL(*producer, Flush(_, _, _, flush_flags))
      .WillRepeatedly(Invoke([&producer, &writer, &flushes_seen, checkpoint](
                                 FlushRequestID flush_req_id,
                                 const DataSourceInstanceID*, size_t,
                                 FlushFlags) {
        {
          auto tp = writer->NewTracePacket();
          char payload[32];
          base::SprintfTrunc(payload, sizeof(payload), "f_%d", flushes_seen);
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
                Contains(Property(&protos::gen::TracePacket::for_testing,
                                  Property(&protos::gen::TestEvent::str,
                                           Eq("f_" + std::to_string(i))))));
  }
}

TEST_F(TracingServiceImplTest, NoFlush) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer_1 = CreateMockProducer();
  producer_1->Connect(svc.get(), "mock_producer_1");
  producer_1->RegisterDataSource("ds_flush");
  producer_1->RegisterDataSource("ds_noflush", false, false, false, true);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_flush");
  trace_config.add_data_sources()->mutable_config()->set_name("ds_noflush");

  consumer->EnableTracing(trace_config);
  producer_1->WaitForTracingSetup();
  producer_1->WaitForDataSourceSetup("ds_flush");
  producer_1->WaitForDataSourceSetup("ds_noflush");
  producer_1->WaitForDataSourceStart("ds_flush");
  producer_1->WaitForDataSourceStart("ds_noflush");

  std::unique_ptr<MockProducer> producer_2 = CreateMockProducer();
  producer_2->Connect(svc.get(), "mock_producer_2");
  producer_2->RegisterDataSource("ds_noflush", false, false, false,
                                 /*no_flush=*/true);
  producer_2->WaitForTracingSetup();
  producer_2->WaitForDataSourceSetup("ds_noflush");
  producer_2->WaitForDataSourceStart("ds_noflush");

  auto wr_p1_ds1 = producer_1->CreateTraceWriter("ds_flush");
  producer_1->ExpectFlush(wr_p1_ds1.get());

  EXPECT_CALL(*producer_2, Flush(_, _, _, _)).Times(0);

  auto flush_request = consumer->Flush();
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
}

TEST_F(TracingServiceImplTest, PeriodicClearIncrementalState) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  // Incremental data source that expects to receive the clear.
  producer->RegisterDataSource("ds_incremental1", false, false,
                               /*handles_incremental_state_clear=*/true);

  // Incremental data source that expects to receive the clear.
  producer->RegisterDataSource("ds_incremental2", false, false,
                               /*handles_incremental_state_clear=*/true);

  // Data source that does *not* advertise itself as supporting incremental
  // state clears.
  producer->RegisterDataSource("ds_selfcontained", false, false,
                               /*handles_incremental_state_clear=*/false);

  // Incremental data source that is registered, but won't be active within the
  // test's tracing session.
  producer->RegisterDataSource("ds_inactive", false, false,
                               /*handles_incremental_state_clear=*/true);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.mutable_incremental_state_config()->set_clear_period_ms(1);
  trace_config.add_data_sources()->mutable_config()->set_name(
      "ds_selfcontained");
  trace_config.add_data_sources()->mutable_config()->set_name(
      "ds_incremental1");
  trace_config.add_data_sources()->mutable_config()->set_name(
      "ds_incremental2");

  // note: the mocking is very brittle, and has to assume a specific order of
  // the data sources' setup/start.
  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_selfcontained");
  producer->WaitForDataSourceSetup("ds_incremental1");
  producer->WaitForDataSourceSetup("ds_incremental2");
  producer->WaitForDataSourceStart("ds_selfcontained");
  producer->WaitForDataSourceStart("ds_incremental1");
  producer->WaitForDataSourceStart("ds_incremental2");

  DataSourceInstanceID ds_incremental1 =
      producer->GetDataSourceInstanceId("ds_incremental1");
  DataSourceInstanceID ds_incremental2 =
      producer->GetDataSourceInstanceId("ds_incremental2");

  const size_t kNumClears = 3;
  std::function<void()> checkpoint =
      task_runner.CreateCheckpoint("clears_received");
  std::vector<std::vector<DataSourceInstanceID>> clears_seen;
  EXPECT_CALL(*producer, ClearIncrementalState(_, _))
      .WillRepeatedly(Invoke([&clears_seen, &checkpoint](
                                 const DataSourceInstanceID* data_source_ids,
                                 size_t num_data_sources) {
        std::vector<DataSourceInstanceID> ds_ids;
        for (size_t i = 0; i < num_data_sources; i++) {
          ds_ids.push_back(*data_source_ids++);
        }
        clears_seen.push_back(ds_ids);
        if (clears_seen.size() >= kNumClears)
          checkpoint();
      }));
  task_runner.RunUntilCheckpoint("clears_received");

  consumer->DisableTracing();

  // Assert that the clears were only for the active incremental data sources.
  ASSERT_EQ(clears_seen.size(), kNumClears);
  for (const std::vector<DataSourceInstanceID>& ds_ids : clears_seen) {
    ASSERT_THAT(ds_ids, ElementsAreArray({ds_incremental1, ds_incremental2}));
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

  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_will_ack_1");
  producer->WaitForDataSourceSetup("ds_wont_ack");
  producer->WaitForDataSourceSetup("ds_will_ack_2");

  DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds_will_ack_1");
  DataSourceInstanceID id2 = producer->GetDataSourceInstanceId("ds_will_ack_2");

  consumer->StartTracing();

  producer->WaitForDataSourceStart("ds_will_ack_1");
  producer->WaitForDataSourceStart("ds_wont_ack");
  producer->WaitForDataSourceStart("ds_will_ack_2");

  producer->endpoint()->NotifyDataSourceStarted(id1);

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("ds_wont_ack");
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("ds_will_ack_1");
  producer->WaitForDataSourceStop("ds_wont_ack");
  producer->WaitForDataSourceStop("ds_will_ack_2");

  producer->endpoint()->NotifyDataSourceStopped(id1);
  producer->endpoint()->NotifyDataSourceStopped(id2);

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
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source", /*ack_stop=*/true);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("data_source");
  trace_config.set_duration_ms(1);
  trace_config.set_data_source_stop_timeout_ms(1);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  producer->ExpectFlush(writer.get());

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

  InSequence seq;
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
    producer1->ExpectFlush(writer1.get());

    auto writer2 = producer2->CreateTraceWriter("ds_2A");
    producer2->ExpectFlush(writer2.get());

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
  trace_config.set_file_write_period_ms(100);
  trace_config.mutable_builtin_data_sources()->set_snapshot_interval_ms(100);
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
    std::string payload(static_cast<size_t>(i),
                        'A' + static_cast<char>(i % 25));
    writer->NewTracePacket()->set_for_testing()->set_str(payload.c_str());
    if (i % (100 / kNumMarkers) == 0) {
      writer->Flush();
      // The snapshot will happen every 100ms
      AdvanceTimeAndRunUntilIdle(100);
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
  std::string merged_trace_raw;
  for (size_t pos = 0; pos != std::string::npos; start = end) {
    pos = trace_raw.find(kSyncMarkerStr, pos + 1);
    num_markers++;
    end = (pos == std::string::npos) ? trace_raw.size() : pos + kMarkerSize;
    size_t size = end - start;
    ASSERT_GT(size, 0u);
    std::string trace_partition_raw = trace_raw.substr(start, size);
    protos::gen::Trace trace_partition;
    ASSERT_TRUE(trace_partition.ParseFromString(trace_partition_raw));
    merged_trace_raw += trace_partition_raw;
  }
  EXPECT_GE(num_markers, static_cast<size_t>(kNumMarkers));

  protos::gen::Trace whole_trace;
  ASSERT_TRUE(whole_trace.ParseFromString(trace_raw));

  protos::gen::Trace merged_trace;
  merged_trace.ParseFromString(merged_trace_raw);

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

  auto writer = producer->CreateTraceWriter("ds_1");
  producer->ExpectFlush(writer.get());

  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ProducerUIDsAndPacketSequenceIDs) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer1", 123u /* uid */,
                     1001 /* pid */);
  producer1->RegisterDataSource("data_source");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer2", 456u /* uid */,
                     2002 /* pid */);
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
  producer1->ExpectFlush({writer1a.get(), writer1b.get()});
  producer2->ExpectFlush(writer2a.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer1->WaitForDataSourceStop("data_source");
  producer2->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::gen::TracePacket::for_testing,
                   Property(&protos::gen::TestEvent::str, Eq("payload1a1"))),
          Property(&protos::gen::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::gen::TracePacket::trusted_pid, Eq(1001)),
          Property(&protos::gen::TracePacket::trusted_packet_sequence_id,
                   Eq(2u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::gen::TracePacket::for_testing,
                   Property(&protos::gen::TestEvent::str, Eq("payload1a2"))),
          Property(&protos::gen::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::gen::TracePacket::trusted_pid, Eq(1001)),
          Property(&protos::gen::TracePacket::trusted_packet_sequence_id,
                   Eq(2u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::gen::TracePacket::for_testing,
                   Property(&protos::gen::TestEvent::str, Eq("payload1b1"))),
          Property(&protos::gen::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::gen::TracePacket::trusted_pid, Eq(1001)),
          Property(&protos::gen::TracePacket::trusted_packet_sequence_id,
                   Eq(3u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::gen::TracePacket::for_testing,
                   Property(&protos::gen::TestEvent::str, Eq("payload1b2"))),
          Property(&protos::gen::TracePacket::trusted_uid, Eq(123)),
          Property(&protos::gen::TracePacket::trusted_pid, Eq(1001)),
          Property(&protos::gen::TracePacket::trusted_packet_sequence_id,
                   Eq(3u)))));
  EXPECT_THAT(
      packets,
      Contains(AllOf(
          Property(&protos::gen::TracePacket::for_testing,
                   Property(&protos::gen::TestEvent::str, Eq("payload2a1"))),
          Property(&protos::gen::TracePacket::trusted_uid, Eq(456)),
          Property(&protos::gen::TracePacket::trusted_pid, Eq(2002)),
          Property(&protos::gen::TracePacket::trusted_packet_sequence_id,
                   Eq(4u)))));
}

#if !PERFETTO_DCHECK_IS_ON()
TEST_F(TracingServiceImplTest, CommitToForbiddenBufferIsDiscarded) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer_2");
  producer2->RegisterDataSource("data_source_2");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  ds_config->set_target_buffer(0);
  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source_2");
  ds_config->set_target_buffer(1);
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");

  producer2->WaitForTracingSetup();
  producer2->WaitForDataSourceSetup("data_source_2");

  producer->WaitForDataSourceStart("data_source");
  producer2->WaitForDataSourceStart("data_source_2");

  const auto* ds1 = producer->GetDataSourceInstance("data_source");
  ASSERT_NE(ds1, nullptr);
  const auto* ds2 = producer2->GetDataSourceInstance("data_source_2");
  ASSERT_NE(ds2, nullptr);
  BufferID buf0 = ds1->target_buffer;
  BufferID buf1 = ds2->target_buffer;

  // Try to write to the correct buffer.
  std::unique_ptr<TraceWriter> writer =
      producer->endpoint()->CreateTraceWriter(buf0);
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("good_payload");
  }

  auto flush_request = consumer->Flush();
  EXPECT_CALL(*producer, Flush)
      .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                           const DataSourceInstanceID*, size_t, FlushFlags) {
        writer->Flush();
        producer->endpoint()->NotifyFlushComplete(flush_req_id);
      }));
  EXPECT_CALL(*producer2, Flush)
      .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                           const DataSourceInstanceID*, size_t, FlushFlags) {
        producer2->endpoint()->NotifyFlushComplete(flush_req_id);
      }));
  ASSERT_TRUE(flush_request.WaitForReply());

  // Try to write to the wrong buffer.
  writer = producer->endpoint()->CreateTraceWriter(buf1);
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("bad_payload");
  }

  flush_request = consumer->Flush();
  EXPECT_CALL(*producer, Flush)
      .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                           const DataSourceInstanceID*, size_t, FlushFlags) {
        writer->Flush();
        producer->endpoint()->NotifyFlushComplete(flush_req_id);
      }));
  EXPECT_CALL(*producer2, Flush)
      .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                           const DataSourceInstanceID*, size_t, FlushFlags) {
        producer2->endpoint()->NotifyFlushComplete(flush_req_id);
      }));

  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  producer2->WaitForDataSourceStop("data_source_2");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("good_payload")))));
  EXPECT_THAT(packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("bad_payload"))))));

  consumer->FreeBuffers();
}
#endif  // !PERFETTO_DCHECK_IS_ON()

TEST_F(TracingServiceImplTest, ScrapeBuffersOnFlush) {
  svc->SetSMBScrapingEnabled(true);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
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

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  // Wait for the writer to be registered.
  task_runner.RunUntilIdle();

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  // Flush but don't actually flush the chunk from TraceWriter.
  auto flush_request = consumer->Flush();
  producer->ExpectFlush(nullptr, /*reply=*/true);
  ASSERT_TRUE(flush_request.WaitForReply());

  // Chunk with the packets should have been scraped.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload1")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload2")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload3")))));

  // Write some more packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload4");
  writer->NewTracePacket()->set_for_testing()->set_str("payload5");

  // Don't reply to flush, causing a timeout. This should scrape again.
  flush_request = consumer->Flush(/*timeout=*/100);
  producer->ExpectFlush(nullptr, /*reply=*/false);
  ASSERT_FALSE(flush_request.WaitForReply());

  // Chunk with the packets should have been scraped again, overriding the
  // original one. The first three should not be read twice.
  packets = consumer->ReadBuffers();
  EXPECT_THAT(packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1"))))));
  EXPECT_THAT(packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload2"))))));
  EXPECT_THAT(packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload3"))))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload4")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload5")))));

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ScrapeBuffersFromAnotherThread) {
  // This test verifies that there are no reported TSAN races while scraping
  // buffers from a producer which is actively writing more trace data
  // concurrently.
  svc->SetSMBScrapingEnabled(true);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
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

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source", BufferExhaustedPolicy::kDrop);
  // Wait for the writer to be registered.
  task_runner.RunUntilIdle();

  std::atomic<bool> packets_written = false;
  std::atomic<bool> quit = false;
  std::thread writer_thread([&] {
    while (!quit.load(std::memory_order_acquire)) {
      writer->NewTracePacket()->set_for_testing()->set_str("payload");
      packets_written.store(true, std::memory_order_release);
      std::this_thread::yield();
    }
  });

  // Wait until the thread has had some time to write some packets.
  while (packets_written.load(std::memory_order_acquire) == false)
    std::this_thread::yield();

  // Disabling tracing will trigger scraping.
  consumer->DisableTracing();

  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  quit.store(true, std::memory_order_release);
  writer_thread.join();

  // Because we don't synchronize with the producer thread, we can't make any
  // guarantees about the number of packets we will successfully read. We just
  // verify that no TSAN races are reported.
  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload")))));
}

// Test scraping on producer disconnect.
TEST_F(TracingServiceImplTest, ScrapeBuffersOnProducerDisconnect) {
  svc->SetSMBScrapingEnabled(true);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();

  static constexpr size_t kShmSizeBytes = 1024 * 1024;
  static constexpr size_t kShmPageSizeBytes = 4 * 1024;

  TestSharedMemory::Factory factory;
  auto shm = factory.CreateSharedMemory(kShmSizeBytes);

  // Service should adopt the SMB provided by the producer.
  producer->Connect(svc.get(), "mock_producer", /*uid=*/42, /*pid=*/1025,
                    /*shared_memory_size_hint_bytes=*/0, kShmPageSizeBytes,
                    TestRefSharedMemory::Create(shm.get()),
                    /*in_process=*/false);

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

  auto client_producer_endpoint = std::make_unique<ProxyProducerEndpoint>();
  client_producer_endpoint->set_backend(producer->endpoint());

  auto shmem_arbiter = std::make_unique<SharedMemoryArbiterImpl>(
      shm->start(), shm->size(), SharedMemoryABI::ShmemMode::kDefault,
      kShmPageSizeBytes, client_producer_endpoint.get(), &task_runner);
  shmem_arbiter->SetDirectSMBPatchingSupportedByService();

  const auto* ds_inst = producer->GetDataSourceInstance("data_source");
  ASSERT_NE(nullptr, ds_inst);
  std::unique_ptr<TraceWriter> writer =
      shmem_arbiter->CreateTraceWriter(ds_inst->target_buffer);
  // Wait for the TraceWriter to be registered.
  task_runner.RunUntilIdle();

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  // Disconnect the producer without committing the chunk. This should cause a
  // scrape of the SMB.
  client_producer_endpoint->set_backend(nullptr);
  producer.reset();

  // Chunk with the packets should have been scraped.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload1")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload2")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload3")))));

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

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  // Wait for the TraceWriter to be registered.
  task_runner.RunUntilIdle();

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  // Chunk with the packets should have been scraped.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload1")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload2")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload3")))));
}

// Fixture for testing scraping from a single data source that writes directly
// to the shared memory, to cover all cases.
class TracingServiceImplScrapingWithSmbTest : public TracingServiceImplTest {
 public:
  void SetUp() override {
    TracingServiceImplTest::SetUp();
    svc->SetSMBScrapingEnabled(true);

    consumer_ = CreateMockConsumer();
    consumer_->Connect(svc.get());
    producer_ = CreateMockProducer();

    static constexpr size_t kShmSizeBytes = 1024 * 1024;
    static constexpr size_t kShmPageSizeBytes = 4 * 1024;

    TestSharedMemory::Factory factory;
    shm_ = factory.CreateSharedMemory(kShmSizeBytes);

    // Service should adopt the SMB provided by the producer.
    producer_->Connect(svc.get(), "mock_producer", /*uid=*/42, /*pid=*/1025,
                       /*shared_memory_size_hint_bytes=*/0, kShmPageSizeBytes,
                       TestRefSharedMemory::Create(shm_.get()),
                       /*in_process=*/false);

    producer_->RegisterDataSource("data_source");

    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(128);
    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("data_source");
    ds_config->set_target_buffer(0);
    consumer_->EnableTracing(trace_config);

    producer_->WaitForTracingSetup();
    producer_->WaitForDataSourceSetup("data_source");
    producer_->WaitForDataSourceStart("data_source");

    arbiter_ = std::make_unique<SharedMemoryArbiterImpl>(
        shm_->start(), shm_->size(), SharedMemoryABI::ShmemMode::kDefault,
        kShmPageSizeBytes, producer_->endpoint(), &task_runner);
    arbiter_->SetDirectSMBPatchingSupportedByService();

    const auto* ds = producer_->GetDataSourceInstance("data_source");
    ASSERT_NE(ds, nullptr);

    target_buffer_ = ds->target_buffer;

    writer_ = arbiter_->CreateTraceWriter(target_buffer_);
    // Wait for the writer to be registered.
    task_runner.RunUntilIdle();
  }

  void TearDown() override {
    TracingServiceImplTest::TearDown();

    consumer_->DisableTracing();
    producer_->WaitForDataSourceStop("data_source");
    consumer_->WaitForTracingDisabled();
  }

 protected:
  std::optional<std::vector<protos::gen::TracePacket>> FlushAndRead() {
    // Scrape: ask the service to flush but don't flush the chunk.
    auto flush_request = consumer_->Flush();

    EXPECT_CALL(*producer_, Flush)
        .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                             const DataSourceInstanceID*, size_t, FlushFlags) {
          arbiter_->NotifyFlushComplete(flush_req_id);
        }));
    if (flush_request.WaitForReply()) {
      return consumer_->ReadBuffers();
    }
    return std::nullopt;
  }
  std::unique_ptr<MockConsumer> consumer_;
  std::unique_ptr<SharedMemory> shm_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
  std::unique_ptr<MockProducer> producer_;
  std::unique_ptr<TraceWriter> writer_;
  BufferID target_buffer_{};

  struct : public protozero::ScatteredStreamWriter::Delegate {
    protozero::ContiguousMemoryRange GetNewBuffer() override {
      PERFETTO_FATAL("Unreachable");
    }

    uint8_t* AnnotatePatch(uint8_t*) override { PERFETTO_FATAL("Unreachable"); }
  } empty_delegate_;
  PatchList empty_patch_list_;
};

TEST_F(TracingServiceImplScrapingWithSmbTest, ScrapeAfterInflatedCount) {
  SharedMemoryABI::ChunkHeader header = {};
  header.writer_id.store(writer_->writer_id(), std::memory_order_relaxed);
  header.chunk_id.store(0, std::memory_order_relaxed);
  header.packets.store({}, std::memory_order_relaxed);

  SharedMemoryABI::Chunk chunk =
      arbiter_->GetNewChunk(header, BufferExhaustedPolicy::kDrop);
  ASSERT_TRUE(chunk.is_valid());

  protozero::ScatteredStreamWriter stream_writer(&empty_delegate_);
  stream_writer.Reset({chunk.payload_begin(), chunk.end()});

  chunk.IncrementPacketCount();

  perfetto::protos::pbzero::TracePacket trace_packet;
  protozero::MessageArena arena;
  trace_packet.Reset(&stream_writer, &arena);
  trace_packet.set_size_field(stream_writer.ReserveBytes(4));

  trace_packet.set_for_testing()->set_str("payload1");

  trace_packet.Finalize();

  auto packets = FlushAndRead();
  ASSERT_TRUE(packets.has_value());
  // The scraping should not have seen the packet.
  EXPECT_THAT(*packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1"))))));

  // Inflate the packet count: this is what
  // TraceWriterImpl::FinishTracePacket() does.
  chunk.IncrementPacketCount();

  packets = FlushAndRead();
  ASSERT_TRUE(packets.has_value());
  // The scraping now should see the packet.
  EXPECT_THAT(*packets,
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1")))));

  // Before marking the chunk as complete, the trace writer writes an empty
  // trace packet (a single byte with zero size), to account for the inflated
  // trace count.
  ASSERT_GT(stream_writer.bytes_available(), 0u);
  uint8_t zero_size = 0;
  stream_writer.WriteBytesUnsafe(&zero_size, sizeof zero_size);

  packets = FlushAndRead();
  ASSERT_TRUE(packets.has_value());
  // The past scraping has already seen the packet.
  EXPECT_THAT(*packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1"))))));

  arbiter_->ReturnCompletedChunk(std::move(chunk), target_buffer_,
                                 &empty_patch_list_);

  packets = FlushAndRead();
  ASSERT_TRUE(packets.has_value());
  // The past scraping has already seen the packet.
  EXPECT_THAT(*packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1"))))));
}

TEST_F(TracingServiceImplScrapingWithSmbTest, ScrapeAfterCompleteChunk) {
  SharedMemoryABI::ChunkHeader header = {};
  header.writer_id.store(writer_->writer_id(), std::memory_order_relaxed);
  header.chunk_id.store(0, std::memory_order_relaxed);
  header.packets.store({}, std::memory_order_relaxed);

  SharedMemoryABI::Chunk chunk =
      arbiter_->GetNewChunk(header, BufferExhaustedPolicy::kDrop);
  ASSERT_TRUE(chunk.is_valid());

  protozero::ScatteredStreamWriter stream_writer(&empty_delegate_);
  stream_writer.Reset({chunk.payload_begin(), chunk.end()});

  chunk.IncrementPacketCount();

  perfetto::protos::pbzero::TracePacket trace_packet;
  protozero::MessageArena arena;
  trace_packet.Reset(&stream_writer, &arena);
  trace_packet.set_size_field(stream_writer.ReserveBytes(4));

  trace_packet.set_for_testing()->set_str("payload1");

  trace_packet.Finalize();

  auto packets = FlushAndRead();
  ASSERT_TRUE(packets.has_value());
  // The scraping should not have seen the packet.
  EXPECT_THAT(*packets,
              Not(Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1"))))));

  // Inflate the packet count: this is what
  // TraceWriterImpl::FinishTracePacket() does.
  chunk.IncrementPacketCount();

  // Before marking the chunk as complete, the trace writer writes an empty
  // trace packet (a single byte with zero size), to account for the inflated
  // trace count.
  ASSERT_GT(stream_writer.bytes_available(), 0u);
  uint8_t zero_size = 0;
  stream_writer.WriteBytesUnsafe(&zero_size, sizeof zero_size);

  arbiter_->ReturnCompletedChunk(std::move(chunk), target_buffer_,
                                 &empty_patch_list_);

  packets = FlushAndRead();
  ASSERT_TRUE(packets.has_value());
  // The chunk has been marked as completed. Flushing should see the packet.
  EXPECT_THAT(*packets,
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload1")))));
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

  // The trace is aborted immediately, the default timeout here is just some
  // slack for the thread ping-pongs for slow devices.
  consumer->WaitForTracingDisabled();
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

TEST_F(TracingServiceImplTest, TraceWriterStats) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source_1");
  producer->RegisterDataSource("data_source_2");

  TraceConfig trace_config;
  for (uint32_t i = 0; i < 3; i++)
    trace_config.add_buffers()->set_size_kb(512);
  for (uint32_t i = 1; i <= 2; i++) {
    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("data_source_" + std::to_string(i));
    ds_config->set_target_buffer(i);  // DS1 : buf[1], DS2: buf[2].
    // buf[0] is deliberately unused, to check we get the buffer_idx right.
  }

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source_1");
  producer->WaitForDataSourceSetup("data_source_2");
  producer->WaitForDataSourceStart("data_source_1");
  producer->WaitForDataSourceStart("data_source_2");

  const std::string payload_128(128 - 32, 'a');
  const std::string payload_512(512 - 32, 'b');
  const std::string payload_1k(1024 - 32, 'c');
  const std::string payload_2k(2048 - 32, 'd');

  auto writer1 = producer->CreateTraceWriter("data_source_1");
  auto writer2 = producer->CreateTraceWriter("data_source_2");

  // Flush after each packet to create chunks that match packets.
  writer1->NewTracePacket()->set_for_testing()->set_str(payload_128);
  writer1->Flush();

  writer1->NewTracePacket()->set_for_testing()->set_str(payload_1k);
  writer1->Flush();

  writer2->NewTracePacket()->set_for_testing()->set_str(payload_512);
  writer2->Flush();

  writer2->NewTracePacket()->set_for_testing()->set_str(payload_2k);
  writer2->Flush();

  writer2->NewTracePacket()->set_for_testing()->set_str(payload_2k);
  writer2->Flush();

  auto flush_request = consumer->Flush();
  producer->ExpectFlush({writer1.get(), writer2.get()});
  ASSERT_TRUE(flush_request.WaitForReply());

  writer1.reset();
  writer2.reset();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source_1");
  producer->WaitForDataSourceStop("data_source_2");
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      Contains(Property(&protos::gen::TracePacket::has_trace_stats, Eq(true))));
  for (const auto& packet : packets) {
    if (!packet.has_trace_stats())
      continue;

    EXPECT_GT(packet.trace_stats().writer_stats().size(), 0u);
    for (const auto& wri : packet.trace_stats().writer_stats()) {
      for (size_t i = 0; i < wri.chunk_payload_histogram_counts().size() - 1;
           i++) {
        PERFETTO_DLOG("Seq=%" PRIu64 ", %" PRIu64 " : %" PRIu64,
                      wri.sequence_id(),
                      packet.trace_stats().chunk_payload_histogram_def()[i],
                      wri.chunk_payload_histogram_counts()[i]);
      }

      switch (wri.sequence_id()) {
        case 1:  // Ignore service-generated packets.
          continue;
        case 2:  // writer1
          EXPECT_EQ(wri.buffer(), 1u);
          EXPECT_THAT(wri.chunk_payload_histogram_counts(),
                      ElementsAreArray({0 /*8*/, 0 /*32*/, 1 /*128*/, 0 /*512*/,
                                        1 /*1K*/, 0 /*2K*/, 0 /*4K*/, 0 /*8K*/,
                                        0 /*12K*/, 0 /*16K*/, 0 /*>16K*/}));
          continue;
        case 3:  // writer2
          EXPECT_EQ(wri.buffer(), 2u);
          EXPECT_THAT(wri.chunk_payload_histogram_counts(),
                      ElementsAreArray({0 /*8*/, 0 /*32*/, 0 /*128*/, 1 /*512*/,
                                        0 /*1K*/, 2 /*2K*/, 0 /*4K*/, 0 /*8K*/,
                                        0 /*12K*/, 0 /*16K*/, 0 /*>16K*/}));
          continue;
        default:
          ASSERT_TRUE(false) << "Unexpected sequence " << wri.sequence_id();
      }
    }
  }
}

TEST_F(TracingServiceImplTest, ObserveEventsDataSourceInstances) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  // Start tracing before the consumer is interested in events. The consumer's
  // OnObservableEvents() should not be called yet.
  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling ObserveEvents should cause an event for the initial instance state.
  auto on_observable_events =
      task_runner.CreateCheckpoint("on_observable_events");
  EXPECT_CALL(*consumer, OnObservableEvents)
      .WillOnce(Invoke([on_observable_events](const ObservableEvents& events) {
        ObservableEvents::DataSourceInstanceStateChange change;
        change.set_producer_name("mock_producer");
        change.set_data_source_name("data_source");
        change.set_state(ObservableEvents::DATA_SOURCE_INSTANCE_STATE_STARTED);
        EXPECT_THAT(events.instance_state_changes(), ElementsAre(change));
        on_observable_events();
      }));

  consumer->ObserveEvents(ObservableEvents::TYPE_DATA_SOURCES_INSTANCES);

  task_runner.RunUntilCheckpoint("on_observable_events");

  // Disabling should cause an instance state change to STOPPED.
  on_observable_events = task_runner.CreateCheckpoint("on_observable_events_2");
  EXPECT_CALL(*consumer, OnObservableEvents)
      .WillOnce(Invoke([on_observable_events](const ObservableEvents& events) {
        ObservableEvents::DataSourceInstanceStateChange change;
        change.set_producer_name("mock_producer");
        change.set_data_source_name("data_source");
        change.set_state(ObservableEvents::DATA_SOURCE_INSTANCE_STATE_STOPPED);
        EXPECT_THAT(events.instance_state_changes(), ElementsAre(change));
        on_observable_events();
      }));
  consumer->DisableTracing();

  producer->WaitForDataSourceStop("data_source");

  consumer->WaitForTracingDisabled();
  task_runner.RunUntilCheckpoint("on_observable_events_2");

  consumer->FreeBuffers();

  // Enable again, this should cause a state change for a new instance to
  // its initial state STOPPED.
  on_observable_events = task_runner.CreateCheckpoint("on_observable_events_3");
  EXPECT_CALL(*consumer, OnObservableEvents)
      .WillOnce(Invoke([on_observable_events](const ObservableEvents& events) {
        ObservableEvents::DataSourceInstanceStateChange change;
        change.set_producer_name("mock_producer");
        change.set_data_source_name("data_source");
        change.set_state(ObservableEvents::DATA_SOURCE_INSTANCE_STATE_STOPPED);
        EXPECT_THAT(events.instance_state_changes(), ElementsAre(change));
        on_observable_events();
      }));

  trace_config.set_deferred_start(true);
  consumer->EnableTracing(trace_config);

  producer->WaitForDataSourceSetup("data_source");
  task_runner.RunUntilCheckpoint("on_observable_events_3");

  // Should move the instance into STARTED state and thus cause an event.
  on_observable_events = task_runner.CreateCheckpoint("on_observable_events_4");
  EXPECT_CALL(*consumer, OnObservableEvents)
      .WillOnce(Invoke([on_observable_events](const ObservableEvents& events) {
        ObservableEvents::DataSourceInstanceStateChange change;
        change.set_producer_name("mock_producer");
        change.set_data_source_name("data_source");
        change.set_state(ObservableEvents::DATA_SOURCE_INSTANCE_STATE_STARTED);
        EXPECT_THAT(events.instance_state_changes(), ElementsAre(change));
        on_observable_events();
      }));
  consumer->StartTracing();

  producer->WaitForDataSourceStart("data_source");
  task_runner.RunUntilCheckpoint("on_observable_events_4");

  // Stop observing events.
  consumer->ObserveEvents(0);

  // Disabling should now no longer cause events to be sent to the consumer.
  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ObserveEventsDataSourceInstancesUnregister) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  // Start tracing before the consumer is interested in events. The consumer's
  // OnObservableEvents() should not be called yet.
  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Calling ObserveEvents should cause an event for the initial instance state.
  consumer->ObserveEvents(ObservableEvents::TYPE_DATA_SOURCES_INSTANCES);
  {
    ObservableEvents event;
    ObservableEvents::DataSourceInstanceStateChange* change =
        event.add_instance_state_changes();
    change->set_producer_name("mock_producer");
    change->set_data_source_name("data_source");
    change->set_state(ObservableEvents::DATA_SOURCE_INSTANCE_STATE_STARTED);
    EXPECT_CALL(*consumer, OnObservableEvents(Eq(event)))
        .WillOnce(InvokeWithoutArgs(
            task_runner.CreateCheckpoint("data_source_started")));

    task_runner.RunUntilCheckpoint("data_source_started");
  }
  {
    ObservableEvents event;
    ObservableEvents::DataSourceInstanceStateChange* change =
        event.add_instance_state_changes();
    change->set_producer_name("mock_producer");
    change->set_data_source_name("data_source");
    change->set_state(ObservableEvents::DATA_SOURCE_INSTANCE_STATE_STOPPED);
    EXPECT_CALL(*consumer, OnObservableEvents(Eq(event)))
        .WillOnce(InvokeWithoutArgs(
            task_runner.CreateCheckpoint("data_source_stopped")));
  }
  producer->UnregisterDataSource("data_source");
  producer->WaitForDataSourceStop("data_source");
  task_runner.RunUntilCheckpoint("data_source_stopped");

  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, ObserveAllDataSourceStarted) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds1", /*ack_stop=*/false, /*ack_start=*/true);
  producer->RegisterDataSource("ds2", /*ack_stop=*/false, /*ack_start=*/true);

  TraceConfig trace_config;
  trace_config.set_deferred_start(true);
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("ds1");
  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("ds2");

  for (int repetition = 0; repetition < 3; repetition++) {
    consumer->EnableTracing(trace_config);

    if (repetition == 0)
      producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("ds1");
    producer->WaitForDataSourceSetup("ds2");
    task_runner.RunUntilIdle();

    consumer->ObserveEvents(ObservableEvents::TYPE_ALL_DATA_SOURCES_STARTED);
    consumer->StartTracing();
    producer->WaitForDataSourceStart("ds1");
    producer->WaitForDataSourceStart("ds2");

    DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds1");
    producer->endpoint()->NotifyDataSourceStarted(id1);

    // The notification shouldn't happen yet, ds2 has not acked.
    task_runner.RunUntilIdle();
    Mock::VerifyAndClearExpectations(consumer.get());

    EXPECT_THAT(
        consumer->ReadBuffers(),
        Contains(Property(
            &protos::gen::TracePacket::service_event,
            Property(
                &protos::gen::TracingServiceEvent::all_data_sources_started,
                Eq(false)))));

    DataSourceInstanceID id2 = producer->GetDataSourceInstanceId("ds2");
    producer->endpoint()->NotifyDataSourceStarted(id2);

    // Now the |all_data_sources_started| notification should be sent.

    auto events = consumer->WaitForObservableEvents();
    ObservableEvents::DataSourceInstanceStateChange change;
    EXPECT_TRUE(events.all_data_sources_started());

    // Disabling should cause an instance state change to STOPPED.
    consumer->DisableTracing();
    producer->WaitForDataSourceStop("ds1");
    producer->WaitForDataSourceStop("ds2");
    consumer->WaitForTracingDisabled();

    EXPECT_THAT(
        consumer->ReadBuffers(),
        Contains(Property(
            &protos::gen::TracePacket::service_event,
            Property(
                &protos::gen::TracingServiceEvent::all_data_sources_started,
                Eq(true)))));
    consumer->FreeBuffers();

    task_runner.RunUntilIdle();

    Mock::VerifyAndClearExpectations(consumer.get());
    Mock::VerifyAndClearExpectations(producer.get());
  }
}

TEST_F(TracingServiceImplTest,
       ObserveAllDataSourceStartedWithoutMatchingInstances) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);

  consumer->ObserveEvents(ObservableEvents::TYPE_ALL_DATA_SOURCES_STARTED);

  // EnableTracing() should immediately cause ALL_DATA_SOURCES_STARTED, because
  // there aren't any matching data sources registered.
  consumer->EnableTracing(trace_config);

  auto events = consumer->WaitForObservableEvents();
  ObservableEvents::DataSourceInstanceStateChange change;
  EXPECT_TRUE(events.all_data_sources_started());

  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();

  EXPECT_THAT(
      consumer->ReadBuffers(),
      Contains(Property(
          &protos::gen::TracePacket::service_event,
          Property(&protos::gen::TracingServiceEvent::all_data_sources_started,
                   Eq(true)))));
  consumer->FreeBuffers();

  task_runner.RunUntilIdle();

  Mock::VerifyAndClearExpectations(consumer.get());
}

// Similar to ObserveAllDataSourceStarted, but covers the case of some data
// sources not supporting the |notify_on_start|.
TEST_F(TracingServiceImplTest, ObserveAllDataSourceStartedOnlySomeWillAck) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds1", /*ack_stop=*/false, /*ack_start=*/true);
  producer->RegisterDataSource("ds2_no_ack");

  TraceConfig trace_config;
  trace_config.set_deferred_start(true);
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("ds1");
  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("ds2_no_ack");

  for (int repetition = 0; repetition < 3; repetition++) {
    consumer->EnableTracing(trace_config);

    if (repetition == 0)
      producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("ds1");
    producer->WaitForDataSourceSetup("ds2_no_ack");
    task_runner.RunUntilIdle();

    consumer->ObserveEvents(ObservableEvents::TYPE_ALL_DATA_SOURCES_STARTED);
    consumer->StartTracing();
    producer->WaitForDataSourceStart("ds1");
    producer->WaitForDataSourceStart("ds2_no_ack");

    DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds1");
    producer->endpoint()->NotifyDataSourceStarted(id1);

    auto events = consumer->WaitForObservableEvents();
    ObservableEvents::DataSourceInstanceStateChange change;
    EXPECT_TRUE(events.all_data_sources_started());

    // Disabling should cause an instance state change to STOPPED.
    consumer->DisableTracing();
    producer->WaitForDataSourceStop("ds1");
    producer->WaitForDataSourceStop("ds2_no_ack");
    consumer->FreeBuffers();
    consumer->WaitForTracingDisabled();

    task_runner.RunUntilIdle();
    Mock::VerifyAndClearExpectations(consumer.get());
    Mock::VerifyAndClearExpectations(producer.get());
  }
}

// Similar to ObserveAllDataSourceStarted, but covers the case of no data
// sources supporting the |notify_on_start|. In this case the
// TYPE_ALL_DATA_SOURCES_STARTED notification should be sent immediately after
// calling Start().
TEST_F(TracingServiceImplTest, ObserveAllDataSourceStartedNoAck) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds1_no_ack");
  producer->RegisterDataSource("ds2_no_ack");

  TraceConfig trace_config;
  trace_config.set_deferred_start(true);
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("ds1_no_ack");
  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("ds2_no_ack");

  for (int repetition = 0; repetition < 3; repetition++) {
    consumer->EnableTracing(trace_config);

    if (repetition == 0)
      producer->WaitForTracingSetup();

    producer->WaitForDataSourceSetup("ds1_no_ack");
    producer->WaitForDataSourceSetup("ds2_no_ack");
    task_runner.RunUntilIdle();

    consumer->ObserveEvents(ObservableEvents::TYPE_ALL_DATA_SOURCES_STARTED);
    consumer->StartTracing();
    producer->WaitForDataSourceStart("ds1_no_ack");
    producer->WaitForDataSourceStart("ds2_no_ack");

    auto events = consumer->WaitForObservableEvents();
    ObservableEvents::DataSourceInstanceStateChange change;
    EXPECT_TRUE(events.all_data_sources_started());

    // Disabling should cause an instance state change to STOPPED.
    consumer->DisableTracing();
    producer->WaitForDataSourceStop("ds1_no_ack");
    producer->WaitForDataSourceStop("ds2_no_ack");
    consumer->FreeBuffers();
    consumer->WaitForTracingDisabled();

    task_runner.RunUntilIdle();
    Mock::VerifyAndClearExpectations(consumer.get());
    Mock::VerifyAndClearExpectations(producer.get());
  }
}

TEST_F(TracingServiceImplTest, LifecycleEventSmoke) {
  using TracingServiceEvent = protos::gen::TracingServiceEvent;
  using TracingServiceEventFnPtr = bool (TracingServiceEvent::*)() const;
  auto has_lifecycle_field = [](TracingServiceEventFnPtr ptr) {
    return Contains(Property(&protos::gen::TracePacket::service_event,
                             Property(ptr, Eq(true))));
  };
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("data_source");

  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");
  task_runner.RunUntilIdle();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets,
              has_lifecycle_field(&TracingServiceEvent::tracing_started));
  EXPECT_THAT(packets, has_lifecycle_field(
                           &TracingServiceEvent::all_data_sources_started));
  EXPECT_THAT(packets,
              has_lifecycle_field(
                  &TracingServiceEvent::read_tracing_buffers_completed));

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  auto flush_request = consumer->Flush();
  producer->ExpectFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, has_lifecycle_field(
                           &TracingServiceEvent::all_data_sources_flushed));
  EXPECT_THAT(packets,
              has_lifecycle_field(
                  &TracingServiceEvent::read_tracing_buffers_completed));

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  packets = consumer->ReadBuffers();
  EXPECT_THAT(packets,
              has_lifecycle_field(&TracingServiceEvent::tracing_disabled));
  EXPECT_THAT(packets,
              has_lifecycle_field(
                  &TracingServiceEvent::read_tracing_buffers_completed));
}

TEST_F(TracingServiceImplTest, LifecycleMultipleFlushEventsQueued) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("data_source");

  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");
  task_runner.RunUntilIdle();

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  auto flush_request = consumer->Flush();
  producer->ExpectFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  flush_request = consumer->Flush();
  producer->ExpectFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  auto packets = consumer->ReadBuffers();
  uint32_t flush_started_count = 0;
  uint32_t flush_done_count = 0;
  for (const auto& packet : packets) {
    flush_started_count += packet.service_event().flush_started();
    flush_done_count += packet.service_event().all_data_sources_flushed();
  }
  EXPECT_EQ(flush_started_count, 2u);
  EXPECT_EQ(flush_done_count, 2u);

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, QueryServiceState) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "producer1", /*uid=*/0);

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "producer2", /*uid=*/1002);

  producer1->RegisterDataSource("common_ds");
  producer2->RegisterDataSource("common_ds");

  producer1->RegisterDataSource("p1_ds");
  producer2->RegisterDataSource("p2_ds");

  producer2->RegisterDataSource("common_ds");

  TracingServiceState svc_state = consumer->QueryServiceState();

  EXPECT_EQ(svc_state.producers_size(), 2);
  EXPECT_EQ(svc_state.producers().at(0).id(), 1);
  EXPECT_EQ(svc_state.producers().at(0).name(), "producer1");
  EXPECT_EQ(svc_state.producers().at(0).uid(), 0);
  EXPECT_EQ(svc_state.producers().at(1).id(), 2);
  EXPECT_EQ(svc_state.producers().at(1).name(), "producer2");
  EXPECT_EQ(svc_state.producers().at(1).uid(), 1002);

  EXPECT_EQ(svc_state.data_sources_size(), 5);

  auto count_ds = [&](int32_t producer_id, const std::string& ds_name) {
    int count = 0;
    for (const auto& ds : svc_state.data_sources()) {
      if (ds.producer_id() == producer_id &&
          ds.ds_descriptor().name() == ds_name)
        ++count;
    }
    return count;
  };

  EXPECT_EQ(count_ds(1, "common_ds"), 1);
  EXPECT_EQ(count_ds(1, "p1_ds"), 1);
  EXPECT_EQ(count_ds(2, "common_ds"), 2);
  EXPECT_EQ(count_ds(2, "p2_ds"), 1);

  // Test that descriptors are cleared when a producer disconnects.
  producer1.reset();
  svc_state = consumer->QueryServiceState();

  EXPECT_EQ(svc_state.producers_size(), 1);
  EXPECT_EQ(svc_state.data_sources_size(), 3);

  EXPECT_EQ(count_ds(1, "common_ds"), 0);
  EXPECT_EQ(count_ds(1, "p1_ds"), 0);
  EXPECT_EQ(count_ds(2, "common_ds"), 2);
  EXPECT_EQ(count_ds(2, "p2_ds"), 1);
}

TEST_F(TracingServiceImplTest, UpdateDataSource) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "producer1", /*uid=*/0);

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "producer2", /*uid=*/1002);

  producer1->RegisterTrackEventDataSource({"cat1"}, 1);
  producer2->RegisterTrackEventDataSource({}, 1);
  producer2->RegisterTrackEventDataSource({}, 2);

  // This request should fail because ID=2 is already registered.
  producer2->RegisterTrackEventDataSource({"this_should_fail"}, 2);

  TracingServiceState svc_state = consumer->QueryServiceState();

  auto parse_desc = [](const perfetto::protos::gen::DataSourceDescriptor& dsd) {
    perfetto::protos::gen::TrackEventDescriptor desc;
    auto desc_raw = dsd.track_event_descriptor_raw();
    EXPECT_TRUE(desc.ParseFromArray(desc_raw.data(), desc_raw.size()));
    return desc;
  };

  EXPECT_EQ(svc_state.data_sources_size(), 3);

  EXPECT_EQ(svc_state.data_sources().at(0).producer_id(), 1);
  EXPECT_EQ(svc_state.data_sources().at(0).ds_descriptor().name(),
            "track_event");
  EXPECT_EQ(svc_state.data_sources().at(0).ds_descriptor().id(), 1u);
  auto ted = parse_desc(svc_state.data_sources().at(0).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 1);
  EXPECT_EQ(ted.available_categories()[0].name(), "cat1");

  EXPECT_EQ(svc_state.data_sources().at(1).producer_id(), 2);
  EXPECT_EQ(svc_state.data_sources().at(1).ds_descriptor().name(),
            "track_event");
  EXPECT_EQ(svc_state.data_sources().at(1).ds_descriptor().id(), 1u);
  ted = parse_desc(svc_state.data_sources().at(1).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 0);

  EXPECT_EQ(svc_state.data_sources().at(2).ds_descriptor().id(), 2u);

  // Test that TrackEvent DataSource is updated.
  producer2->UpdateTrackEventDataSource({"cat1", "cat2"}, 2);

  svc_state = consumer->QueryServiceState();

  EXPECT_EQ(svc_state.data_sources_size(), 3);

  EXPECT_EQ(svc_state.data_sources().at(0).producer_id(), 1);
  EXPECT_EQ(svc_state.data_sources().at(0).ds_descriptor().id(), 1u);
  ted = parse_desc(svc_state.data_sources().at(0).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 1);

  EXPECT_EQ(svc_state.data_sources().at(1).ds_descriptor().id(), 1u);
  ted = parse_desc(svc_state.data_sources().at(1).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 0);

  EXPECT_EQ(svc_state.data_sources().at(2).producer_id(), 2);
  EXPECT_EQ(svc_state.data_sources().at(2).ds_descriptor().id(), 2u);
  ted = parse_desc(svc_state.data_sources().at(2).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 2);
  EXPECT_EQ(ted.available_categories()[0].name(), "cat1");
  EXPECT_EQ(ted.available_categories()[1].name(), "cat2");

  // Test removal of a category.
  producer2->UpdateTrackEventDataSource({"cat2"}, 2);

  svc_state = consumer->QueryServiceState();

  EXPECT_EQ(svc_state.data_sources_size(), 3);
  EXPECT_EQ(svc_state.data_sources().at(2).ds_descriptor().id(), 2u);
  ted = parse_desc(svc_state.data_sources().at(2).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 1);
  EXPECT_EQ(ted.available_categories()[0].name(), "cat2");

  // Test adding a category to the first data source.
  producer2->UpdateTrackEventDataSource({"cat3"}, 1);

  svc_state = consumer->QueryServiceState();

  EXPECT_EQ(svc_state.data_sources_size(), 3);
  EXPECT_EQ(svc_state.data_sources().at(1).ds_descriptor().id(), 1u);
  ted = parse_desc(svc_state.data_sources().at(1).ds_descriptor());
  EXPECT_EQ(ted.available_categories_size(), 1);
  EXPECT_EQ(ted.available_categories()[0].name(), "cat3");
}

TEST_F(TracingServiceImplTest, LimitSessionsPerUid) {
  std::vector<std::unique_ptr<MockConsumer>> consumers;

  auto start_new_session = [&](uid_t uid) -> MockConsumer* {
    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(128);
    trace_config.set_duration_ms(0);  // Unlimited.
    consumers.emplace_back(CreateMockConsumer());
    consumers.back()->Connect(svc.get(), uid);
    consumers.back()->EnableTracing(trace_config);
    return &*consumers.back();
  };

  const int kMaxConcurrentTracingSessionsPerUid = 5;
  const int kUids = 2;

  // Create a bunch of legit sessions (2 uids * 5 sessions).
  for (int i = 0; i < kMaxConcurrentTracingSessionsPerUid * kUids; i++) {
    start_new_session(/*uid=*/static_cast<uid_t>(i) % kUids);
  }

  // Any other session now should fail for the two uids.
  for (int i = 0; i <= kUids; i++) {
    auto* consumer = start_new_session(/*uid=*/static_cast<uid_t>(i) % kUids);
    auto on_fail = task_runner.CreateCheckpoint("uid_" + std::to_string(i));
    EXPECT_CALL(*consumer, OnTracingDisabled(StrNe("")))
        .WillOnce(InvokeWithoutArgs(on_fail));
  }

  // Wait for failure (only after both attempts).
  for (int i = 0; i <= kUids; i++) {
    task_runner.RunUntilCheckpoint("uid_" + std::to_string(i));
  }

  // The destruction of |consumers| will tear down and stop the good sessions.
}

TEST_F(TracingServiceImplTest, ProducerProvidedSMB) {
  static constexpr size_t kShmSizeBytes = 1024 * 1024;
  static constexpr size_t kShmPageSizeBytes = 4 * 1024;

  std::unique_ptr<MockProducer> producer = CreateMockProducer();

  TestSharedMemory::Factory factory;
  auto shm = factory.CreateSharedMemory(kShmSizeBytes);
  SharedMemory* shm_raw = shm.get();

  // Service should adopt the SMB provided by the producer.
  producer->Connect(svc.get(), "mock_producer", /*uid=*/42, /*pid=*/1025,
                    /*shared_memory_size_hint_bytes=*/0, kShmPageSizeBytes,
                    std::move(shm));
  EXPECT_TRUE(producer->endpoint()->IsShmemProvidedByProducer());
  EXPECT_NE(producer->endpoint()->MaybeSharedMemoryArbiter(), nullptr);
  EXPECT_EQ(producer->endpoint()->shared_memory(), shm_raw);

  producer->WaitForTracingSetup();
  producer->RegisterDataSource("data_source");

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  producer->WaitForDataSourceSetup("data_source");
  producer->WaitForDataSourceStart("data_source");

  // Verify that data written to the producer-provided SMB ends up in trace
  // buffer correctly.
  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload");
  }

  auto flush_request = consumer->Flush();
  producer->ExpectFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();
  EXPECT_THAT(consumer->ReadBuffers(),
              Contains(Property(
                  &protos::gen::TracePacket::for_testing,
                  Property(&protos::gen::TestEvent::str, Eq("payload")))));
}

TEST_F(TracingServiceImplTest, ProducerProvidedSMBInvalidSizes) {
  static constexpr size_t kShmSizeBytes = 1024 * 1024;
  static constexpr size_t kShmPageSizeBytes = 20 * 1024;

  std::unique_ptr<MockProducer> producer = CreateMockProducer();

  TestSharedMemory::Factory factory;
  auto shm = factory.CreateSharedMemory(kShmSizeBytes);

  // Service should not adopt the SMB provided by the producer, because the SMB
  // size isn't a multiple of the page size.
  producer->Connect(svc.get(), "mock_producer", /*uid=*/42, /*pid=*/1025,
                    /*shared_memory_size_hint_bytes=*/0, kShmPageSizeBytes,
                    std::move(shm));
  EXPECT_FALSE(producer->endpoint()->IsShmemProvidedByProducer());
  EXPECT_EQ(producer->endpoint()->shared_memory(), nullptr);
}

// If the consumer specifies a UUID in the TraceConfig, the TraceUuid packet
// must match that.
TEST_F(TracingServiceImplTest, UuidPacketMatchesConfigUuid) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  TraceConfig trace_config;
  trace_config.set_trace_uuid_lsb(1);
  trace_config.set_trace_uuid_msb(2);
  trace_config.add_buffers()->set_size_kb(8);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();

  EXPECT_THAT(
      packets,
      Contains(Property(&protos::gen::TracePacket::trace_uuid,
                        AllOf(Property(&protos::gen::TraceUuid::lsb, Eq(1)),
                              Property(&protos::gen::TraceUuid::msb, Eq(2))))));
}

// If the consumer does not specify any UUID in the TraceConfig, a random
// UUID must be generated and reported in the TraceUuid packet.
TEST_F(TracingServiceImplTest, RandomUuidIfNoConfigUuid) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(8);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");

  consumer->EnableTracing(trace_config);
  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();

  EXPECT_THAT(packets,
              Contains(Property(
                  &protos::gen::TracePacket::trace_uuid,
                  Not(AnyOf(Property(&protos::gen::TraceUuid::lsb, Eq(0)),
                            Property(&protos::gen::TraceUuid::msb, Eq(0)))))));
}

TEST_F(TracingServiceImplTest, CloneSession) {
  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  // The consumer that clones it and reads back the data.
  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  // Create two data sources, as we'll write on two distinct buffers.
  producer->RegisterDataSource("ds_1");
  producer->RegisterDataSource("ds_2");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);  // Buf 0.
  trace_config.add_buffers()->set_size_kb(32);  // Buf 1.
  trace_config.set_trace_uuid_lsb(4242);
  trace_config.set_trace_uuid_msb(3737);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);
  ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_2");
  ds_cfg->set_target_buffer(1);

  // Add a filter and check that the filter is propagated to the cloned session.
  // The filter allows the `for_testing` field but not the root `timestamp`.
  protozero::FilterBytecodeGenerator filt;
  // Message 0: root Trace proto.
  filt.AddNestedField(1 /* root trace.packet*/, 1);
  filt.EndMessage();
  // Message 1: TracePacket proto. Allow only the `for_testing` and `trace_uuid`
  // sub-fields.
  filt.AddSimpleField(protos::pbzero::TracePacket::kTraceUuidFieldNumber);
  filt.AddSimpleField(protos::pbzero::TracePacket::kForTestingFieldNumber);
  filt.EndMessage();
  trace_config.mutable_trace_filter()->set_bytecode(filt.Serialize());

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceSetup("ds_2");

  producer->WaitForDataSourceStart("ds_1");
  producer->WaitForDataSourceStart("ds_2");

  std::unique_ptr<TraceWriter> writers[] = {
      producer->CreateTraceWriter("ds_1"),
      producer->CreateTraceWriter("ds_2"),
  };

  // Add some data to both buffers.
  static constexpr size_t kNumTestPackets = 20;
  for (size_t i = 0; i < kNumTestPackets; i++) {
    auto tp = writers[i % 1]->NewTracePacket();
    std::string payload("payload" + std::to_string(i));
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
    tp->set_timestamp(static_cast<uint64_t>(i));
  }

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  base::Uuid clone_uuid;
  EXPECT_CALL(*consumer2, OnSessionCloned(_))
      .WillOnce(Invoke(
          [clone_done, &clone_uuid](const Consumer::OnSessionClonedArgs& args) {
            ASSERT_TRUE(args.success);
            ASSERT_TRUE(args.error.empty());
            // Ensure the LSB is preserved, but the MSB is different. See
            // comments in tracing_service_impl.cc and perfetto_cmd.cc around
            // triggering_subscription_id().
            ASSERT_EQ(args.uuid.lsb(), 4242);
            ASSERT_NE(args.uuid.msb(), 3737);
            clone_uuid = args.uuid;
            clone_done();
          }));
  consumer2->CloneSession(1);
  // CloneSession() will implicitly issue a flush. Linearize with that.
  producer->ExpectFlush({writers[0].get(), writers[1].get()});
  task_runner.RunUntilCheckpoint("clone_done");

  // Overwrite the ring buffer of the original session to check that clone
  // actually returns a copy.
  for (size_t i = 0; i < 1000; i++) {
    auto tp = writers[i % 2]->NewTracePacket();
    std::string payload(1000u, 'x');
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
  }

  auto flush_request = consumer->Flush();
  producer->ExpectFlush({writers[0].get(), writers[1].get()});
  ASSERT_TRUE(flush_request.WaitForReply());

  // Delete the initial tracing session.
  consumer->DisableTracing();
  consumer->FreeBuffers();
  producer->WaitForDataSourceStop("ds_1");
  producer->WaitForDataSourceStop("ds_2");
  consumer->WaitForTracingDisabled();

  // Read back the cloned trace and check the contents.
  auto packets = consumer2->ReadBuffers();
  for (size_t i = 0; i < kNumTestPackets; i++) {
    std::string payload = "payload" + std::to_string(i);
    EXPECT_THAT(packets,
                Contains(Property(
                    &protos::gen::TracePacket::for_testing,
                    Property(&protos::gen::TestEvent::str, Eq(payload)))));
  }

  // Check that the "x" payload written after cloning the session is not there.
  EXPECT_THAT(packets,
              Not(Contains(Property(&protos::gen::TracePacket::for_testing,
                                    Property(&protos::gen::TestEvent::str,
                                             testing::StartsWith("x"))))));

  // Check that the `timestamp` field is filtered out.
  EXPECT_THAT(packets,
              Each(Property(&protos::gen::TracePacket::has_timestamp, false)));

  // Check that the UUID in the trace matches the UUID passed to to the
  // OnCloneSession consumer API.
  EXPECT_THAT(
      packets,
      Contains(Property(
          &protos::gen::TracePacket::trace_uuid,
          AllOf(
              Property(&protos::gen::TraceUuid::msb, Eq(clone_uuid.msb())),
              Property(&protos::gen::TraceUuid::lsb, Eq(clone_uuid.lsb()))))));
}

// Test that a consumer cannot clone a session from a consumer with a different
// uid (unless it's marked as eligible for bugreport, see next test).
TEST_F(TracingServiceImplTest, CloneSessionAcrossUidDenied) {
  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  // The consumer that clones it and reads back the data.
  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get(), 1234);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);

  consumer->EnableTracing(trace_config);
  auto flush_request = consumer->Flush();
  ASSERT_TRUE(flush_request.WaitForReply());

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  EXPECT_CALL(*consumer2, OnSessionCloned(_))
      .WillOnce(Invoke([clone_done](const Consumer::OnSessionClonedArgs& args) {
        clone_done();
        ASSERT_FALSE(args.success);
        ASSERT_TRUE(base::Contains(args.error, "session from another UID"));
      }));
  consumer2->CloneSession(1);
  task_runner.RunUntilCheckpoint("clone_done");
}

// Test that a consumer can clone a session from the shell uid if the trace is
// marked as eligible for bugreport. Android only.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
TEST_F(TracingServiceImplTest, CloneSessionAcrossUidForBugreport) {
  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("ds_1");

  // The consumer that clones it and reads back the data.
  std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
  clone_consumer->Connect(svc.get(), AID_SHELL);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);
  trace_config.set_bugreport_score(1);
  trace_config.add_data_sources()->mutable_config()->set_name("ds_1");

  // Add a trace filter and ensure it's ignored for bugreports (b/317065412).
  protozero::FilterBytecodeGenerator filt;
  filt.AddNestedField(1 /* root trace.packet*/, 1);
  filt.EndMessage();
  // Add a random field to keep the generator happy. This technically still
  // filters out the for_testing packet that we are using below.
  filt.AddSimpleField(protos::pbzero::TracePacket::kTraceUuidFieldNumber);
  filt.EndMessage();
  trace_config.mutable_trace_filter()->set_bytecode_v2(filt.Serialize());

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");
  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload");
  writer.reset();

  auto flush_request = consumer->Flush();
  FlushFlags flush_flags(FlushFlags::Initiator::kConsumerSdk,
                         FlushFlags::Reason::kExplicit);
  producer->ExpectFlush({}, /*reply=*/true, flush_flags);
  ASSERT_TRUE(flush_request.WaitForReply());

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  EXPECT_CALL(*clone_consumer, OnSessionCloned(_))
      .WillOnce(Invoke([clone_done](const Consumer::OnSessionClonedArgs& args) {
        clone_done();
        ASSERT_TRUE(args.success);
      }));

  FlushFlags flush_flags2(FlushFlags::Initiator::kTraced,
                          FlushFlags::Reason::kTraceClone,
                          FlushFlags::CloneTarget::kBugreport);
  producer->ExpectFlush({}, /*reply=*/true, flush_flags2);

  clone_consumer->CloneSession(kBugreportSessionId);
  task_runner.RunUntilCheckpoint("clone_done");

  auto packets = clone_consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  HasSubstr("payload")))));
}
#endif  // OS_ANDROID

TEST_F(TracingServiceImplTest, TransferOnClone) {
  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  // Create two data sources, as we'll write on two distinct buffers.
  producer->RegisterDataSource("ds_1");
  producer->RegisterDataSource("ds_2");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);  // Buf 0.
  auto* buf1_cfg = trace_config.add_buffers();    // Buf 1 (transfer_on_clone).
  buf1_cfg->set_size_kb(1024);
  buf1_cfg->set_transfer_on_clone(true);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);
  ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_2");
  ds_cfg->set_target_buffer(1);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceSetup("ds_2");

  producer->WaitForDataSourceStart("ds_1");
  producer->WaitForDataSourceStart("ds_2");

  std::unique_ptr<TraceWriter> writers[] = {
      producer->CreateTraceWriter("ds_1"),
      producer->CreateTraceWriter("ds_2"),
  };

  // Write once in the first buffer. This is expected persist across clones.
  static constexpr int kNumTestPackets = 10;
  for (int n = 0; n < kNumTestPackets; n++) {
    auto tp = writers[0]->NewTracePacket();
    base::StackString<64> payload("persistent_%d", n);
    tp->set_for_testing()->set_str(payload.c_str(), payload.len());
  }

  const int kLastIteration = 3;
  for (int iteration = 1; iteration <= kLastIteration; iteration++) {
    // The consumer the creates the initial tracing session.
    std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
    clone_consumer->Connect(svc.get());

    // Add some new data to the 2nd buffer, which is transferred.
    // Omit the writing the last iteration to test we get an empty buffer.
    for (int n = 0; n < kNumTestPackets && iteration != kLastIteration; n++) {
      auto tp = writers[1]->NewTracePacket();
      base::StackString<64> payload("transferred_%d_%d", iteration, n);
      tp->set_for_testing()->set_str(payload.c_str(), payload.len());
    }

    std::string clone_checkpoint_name = "clone_" + std::to_string(iteration);
    auto clone_done = task_runner.CreateCheckpoint(clone_checkpoint_name);
    base::Uuid clone_uuid;
    EXPECT_CALL(*clone_consumer, OnSessionCloned(_))
        .WillOnce(InvokeWithoutArgs(clone_done));
    clone_consumer->CloneSession(1);

    // CloneSession() will implicitly issue a flush. Linearize with that.
    EXPECT_CALL(
        *producer,
        Flush(_, Pointee(producer->GetDataSourceInstanceId("ds_1")), 1, _))
        .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                             const DataSourceInstanceID*, size_t, FlushFlags) {
          writers[0]->Flush();
          producer->endpoint()->NotifyFlushComplete(flush_req_id);
        }));
    EXPECT_CALL(
        *producer,
        Flush(_, Pointee(producer->GetDataSourceInstanceId("ds_2")), 1, _))
        .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                             const DataSourceInstanceID*, size_t, FlushFlags) {
          writers[1]->Flush();
          producer->endpoint()->NotifyFlushComplete(flush_req_id);
        }));
    task_runner.RunUntilCheckpoint(clone_checkpoint_name);

    auto packets = clone_consumer->ReadBuffers();
    std::vector<std::string> actual_payloads;
    for (const auto& packet : packets) {
      if (packet.has_for_testing())
        actual_payloads.emplace_back(packet.for_testing().str());
    }
    std::vector<std::string> expected_payloads;
    for (int n = 0; n < kNumTestPackets; n++) {
      base::StackString<64> expected_payload("persistent_%d", n);
      expected_payloads.emplace_back(expected_payload.ToStdString());
    }
    for (int n = 0; n < kNumTestPackets && iteration != kLastIteration; n++) {
      base::StackString<64> expected_payload("transferred_%d_%d", iteration, n);
      expected_payloads.emplace_back(expected_payload.ToStdString());
    }
    ASSERT_THAT(actual_payloads, ElementsAreArray(expected_payloads));
  }  // for (iteration)

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds_1");
  producer->WaitForDataSourceStop("ds_2");
  consumer->WaitForTracingDisabled();

  // Read the data from the primary (non-cloned) tracing session. Check that
  // it doesn't have any "transferred_xxx" payload but only the "persistent_xxx"
  // coming from the standard non-transferred buffer.
  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets,
              Not(Contains(Property(&protos::gen::TracePacket::for_testing,
                                    Property(&protos::gen::TestEvent::str,
                                             HasSubstr("transferred_"))))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  HasSubstr("persistent_")))));
}

TEST_F(TracingServiceImplTest, ClearBeforeClone) {
  // The consumer that creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  // Unused. This buffer is created only to make the test less trivial and cover
  // the case of the clear-bufferd to be the beyond the 0th entry.
  trace_config.add_buffers()->set_size_kb(32);

  auto* buf_cfg = trace_config.add_buffers();
  buf_cfg->set_size_kb(1024);
  buf_cfg->set_clear_before_clone(true);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(1);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");

  // These packets, emitted before the clone, should be dropped.
  for (int i = 0; i < 3; i++) {
    writer->NewTracePacket()->set_for_testing()->set_str("before_clone");
  }
  auto flush_request = consumer->Flush();
  producer->ExpectFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
  clone_consumer->Connect(svc.get());

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  EXPECT_CALL(*clone_consumer, OnSessionCloned(_))
      .WillOnce(InvokeWithoutArgs(clone_done));
  clone_consumer->CloneSession(1);

  // CloneSession() will implicitly issue a flush. Write some other packets
  // in that callback. Those are the only ones that should survive in the cloned
  // session.
  FlushFlags flush_flags(FlushFlags::Initiator::kTraced,
                         FlushFlags::Reason::kTraceClone);
  EXPECT_CALL(*producer, Flush(_, _, _, flush_flags))
      .WillOnce(Invoke([&](FlushRequestID flush_req_id,
                           const DataSourceInstanceID*, size_t, FlushFlags) {
        writer->NewTracePacket()->set_for_testing()->set_str("after_clone");
        writer->Flush(
            [&] { producer->endpoint()->NotifyFlushComplete(flush_req_id); });
      }));

  task_runner.RunUntilCheckpoint("clone_done");

  auto packets = clone_consumer->ReadBuffers();
  EXPECT_THAT(packets,
              Not(Contains(Property(&protos::gen::TracePacket::for_testing,
                                    Property(&protos::gen::TestEvent::str,
                                             HasSubstr("before_clone"))))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  HasSubstr("after_clone")))));
}

TEST_F(TracingServiceImplTest, CloneMainSessionStopped) {
  // The consumer that creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer1");
  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);  // Buf 0.
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("before_clone");
  }
  writer->Flush();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  // The tracing session is disabled, but it's still there. We can still clone
  // it.
  std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
  clone_consumer->Connect(svc.get());

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  EXPECT_CALL(*clone_consumer, OnSessionCloned(_))
      .WillOnce(InvokeWithoutArgs(clone_done));
  clone_consumer->CloneSession(1);

  auto packets = clone_consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  HasSubstr("before_clone")))));
}

TEST_F(TracingServiceImplTest, CloneConsumerDisconnect) {
  // The consumer that creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer1");
  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);  // Buf 0.
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer1 = producer->CreateTraceWriter("ds_1");

  std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
  clone_consumer->Connect(svc.get());

  // CloneSession() will issue a flush.
  std::string producer1_flush_checkpoint_name = "producer1_flush_requested";
  FlushRequestID flush1_req_id;
  auto flush1_requested =
      task_runner.CreateCheckpoint(producer1_flush_checkpoint_name);
  EXPECT_CALL(*producer, Flush(_, _, _, _))
      .WillOnce([&](FlushRequestID req_id, const DataSourceInstanceID*, size_t,
                    FlushFlags) {
        flush1_req_id = req_id;
        flush1_requested();
      });
  clone_consumer->CloneSession(1);

  task_runner.RunUntilCheckpoint(producer1_flush_checkpoint_name);

  // producer hasn't replied to the flush yet, so the clone operation is still
  // pending.

  // The clone_consumer disconnect and goes away.
  clone_consumer.reset();

  // producer replies to the flush request now.
  writer1->Flush();
  producer->endpoint()->NotifyFlushComplete(flush1_req_id);
  task_runner.RunUntilIdle();

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, CloneMainSessionGoesAwayDuringFlush) {
  // The consumer that creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer1");
  producer1->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);  // Buf 0.
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  consumer->EnableTracing(trace_config);
  producer1->WaitForTracingSetup();
  producer1->WaitForDataSourceSetup("ds_1");
  producer1->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer1 = producer1->CreateTraceWriter("ds_1");

  {
    auto tp = writer1->NewTracePacket();
    tp->set_for_testing()->set_str("buf1_beforeflush");
  }
  writer1->Flush();

  std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
  clone_consumer->Connect(svc.get());

  std::string clone_done_name = "consumer1_clone_done";
  auto clone_done = task_runner.CreateCheckpoint(clone_done_name);
  EXPECT_CALL(*clone_consumer, OnSessionCloned)
      .Times(1)
      .WillOnce(Invoke([&](const Consumer::OnSessionClonedArgs& args) {
        EXPECT_FALSE(args.success);
        EXPECT_THAT(args.error, HasSubstr("Original session ended"));
        clone_done();
      }));
  clone_consumer->CloneSession(1);

  std::string producer1_flush_checkpoint_name = "producer1_flush_requested";
  auto flush1_requested =
      task_runner.CreateCheckpoint(producer1_flush_checkpoint_name);
  FlushRequestID flush1_req_id;

  // CloneSession() will issue a flush.
  EXPECT_CALL(*producer1, Flush(_, _, _, _))
      .WillOnce([&](FlushRequestID flush_id, const DataSourceInstanceID*,
                    size_t, FlushFlags) {
        flush1_req_id = flush_id;
        flush1_requested();
      });

  task_runner.RunUntilCheckpoint(producer1_flush_checkpoint_name);

  // The main session goes away.
  consumer->DisableTracing();
  producer1->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
  consumer.reset();

  task_runner.RunUntilCheckpoint(clone_done_name);

  // producer1 replies to flush much later.
  producer1->endpoint()->NotifyFlushComplete(flush1_req_id);
  task_runner.RunUntilIdle();
}

TEST_F(TracingServiceImplTest, CloneTransferFlush) {
  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer1 = CreateMockProducer();
  producer1->Connect(svc.get(), "mock_producer1");
  producer1->RegisterDataSource("ds_1");

  std::unique_ptr<MockProducer> producer2 = CreateMockProducer();
  producer2->Connect(svc.get(), "mock_producer2");
  producer2->RegisterDataSource("ds_2");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);  // Buf 0.
  auto* buf1_cfg = trace_config.add_buffers();    // Buf 1 (transfer_on_clone).
  buf1_cfg->set_size_kb(1024);
  buf1_cfg->set_transfer_on_clone(true);
  buf1_cfg->set_clear_before_clone(true);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);
  ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_2");
  ds_cfg->set_target_buffer(1);

  consumer->EnableTracing(trace_config);
  producer1->WaitForTracingSetup();
  producer1->WaitForDataSourceSetup("ds_1");

  producer2->WaitForTracingSetup();
  producer2->WaitForDataSourceSetup("ds_2");

  producer1->WaitForDataSourceStart("ds_1");
  producer2->WaitForDataSourceStart("ds_2");

  std::unique_ptr<TraceWriter> writer1 = producer1->CreateTraceWriter("ds_1");
  std::unique_ptr<TraceWriter> writer2 = producer2->CreateTraceWriter("ds_2");

  {
    auto tp = writer1->NewTracePacket();
    tp->set_for_testing()->set_str("buf1_beforeflush");
  }

  {
    std::unique_ptr<MockConsumer> clone_consumer = CreateMockConsumer();
    clone_consumer->Connect(svc.get());

    {
      auto tp = writer2->NewTracePacket();
      tp->set_for_testing()->set_str("buf2_beforeflush");
    }

    std::string clone_checkpoint_name = "clone";
    auto clone_done = task_runner.CreateCheckpoint(clone_checkpoint_name);
    EXPECT_CALL(*clone_consumer, OnSessionCloned(_))
        .WillOnce(InvokeWithoutArgs(clone_done));
    clone_consumer->CloneSession(1);

    std::string producer1_flush_checkpoint_name = "producer1_flush_requested";
    FlushRequestID flush1_req_id;
    auto flush1_requested =
        task_runner.CreateCheckpoint(producer1_flush_checkpoint_name);
    std::string producer2_flush_checkpoint_name = "producer2_flush_requested";
    FlushRequestID flush2_req_id;
    auto flush2_requested =
        task_runner.CreateCheckpoint(producer2_flush_checkpoint_name);

    // CloneSession() will issue a flush.
    EXPECT_CALL(*producer1, Flush(_, _, _, _))
        .WillOnce([&](FlushRequestID req_id, const DataSourceInstanceID*,
                      size_t, FlushFlags) {
          flush1_req_id = req_id;
          flush1_requested();
        });
    EXPECT_CALL(*producer2, Flush(_, _, _, _))
        .WillOnce([&](FlushRequestID req_id, const DataSourceInstanceID*,
                      size_t, FlushFlags) {
          flush2_req_id = req_id;
          flush2_requested();
        });

    task_runner.RunUntilCheckpoint(producer1_flush_checkpoint_name);
    task_runner.RunUntilCheckpoint(producer2_flush_checkpoint_name);

    // producer1 is fast and replies to the Flush request immediately.
    writer1->Flush();
    producer1->endpoint()->NotifyFlushComplete(flush1_req_id);
    task_runner.RunUntilIdle();

    // producer1 writes another packet, after acking the flush.
    {
      auto tp = writer1->NewTracePacket();
      tp->set_for_testing()->set_str("buf1_afterflush");
    }
    writer1->Flush();

    // producer2 is slower and is still writing data.
    {
      auto tp = writer2->NewTracePacket();
      tp->set_for_testing()->set_str("buf2_afterflush");
    }

    // now producer2 replies to the Flush request.
    writer2->Flush();
    producer2->endpoint()->NotifyFlushComplete(flush2_req_id);
    task_runner.RunUntilCheckpoint(clone_checkpoint_name);

    auto packets = clone_consumer->ReadBuffers();
    std::vector<std::string> actual_payloads;
    for (const auto& packet : packets) {
      if (packet.has_for_testing())
        actual_payloads.emplace_back(packet.for_testing().str());
    }
    EXPECT_THAT(actual_payloads, Contains("buf1_beforeflush"));
    EXPECT_THAT(actual_payloads, Contains("buf2_beforeflush"));
    // This packet was sent after producer1 acked the flush. producer2 hadn't
    // acked the flush yet, but producer2's buffer is on a separate flush group.
    EXPECT_THAT(actual_payloads, Not(Contains("buf1_afterflush")));
    EXPECT_THAT(actual_payloads, Contains("buf2_afterflush"));
  }

  consumer->DisableTracing();
  producer1->WaitForDataSourceStop("ds_1");
  producer2->WaitForDataSourceStop("ds_2");
  consumer->WaitForTracingDisabled();
}

TEST_F(TracingServiceImplTest, CloneSessionByName) {
  // The consumer the creates the initial tracing session.
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  // The consumer that clones it and reads back the data.
  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);
  trace_config.set_unique_session_name("my_unique_session_name");
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");

  static constexpr size_t kNumTestPackets = 20;
  for (size_t i = 0; i < kNumTestPackets; i++) {
    auto tp = writer->NewTracePacket();
    std::string payload("payload" + std::to_string(i));
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
    tp->set_timestamp(static_cast<uint64_t>(i));
  }

  {
    auto clone_done = task_runner.CreateCheckpoint("clone_done");
    EXPECT_CALL(*consumer2, OnSessionCloned(_))
        .WillOnce(
            Invoke([clone_done](const Consumer::OnSessionClonedArgs& args) {
              ASSERT_TRUE(args.success);
              ASSERT_TRUE(args.error.empty());
              clone_done();
            }));
    ConsumerEndpoint::CloneSessionArgs args;
    args.unique_session_name = "my_unique_session_name";
    consumer2->endpoint()->CloneSession(args);
    // CloneSession() will implicitly issue a flush. Linearize with that.
    producer->ExpectFlush(writer.get());
    task_runner.RunUntilCheckpoint("clone_done");
  }

  // Disable the initial tracing session.
  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();

  // Read back the cloned trace and the original trace.
  auto packets = consumer->ReadBuffers();
  auto cloned_packets = consumer2->ReadBuffers();
  for (size_t i = 0; i < kNumTestPackets; i++) {
    std::string payload = "payload" + std::to_string(i);
    EXPECT_THAT(packets,
                Contains(Property(
                    &protos::gen::TracePacket::for_testing,
                    Property(&protos::gen::TestEvent::str, Eq(payload)))));
    EXPECT_THAT(cloned_packets,
                Contains(Property(
                    &protos::gen::TracePacket::for_testing,
                    Property(&protos::gen::TestEvent::str, Eq(payload)))));
  }

  // Delete the original tracing session.
  consumer->FreeBuffers();

  {
    std::unique_ptr<MockConsumer> consumer3 = CreateMockConsumer();
    consumer3->Connect(svc.get());

    // The original session is gone. The cloned session is still there. It
    // should not be possible to clone that by name.

    auto clone_failed = task_runner.CreateCheckpoint("clone_failed");
    EXPECT_CALL(*consumer3, OnSessionCloned(_))
        .WillOnce(
            Invoke([clone_failed](const Consumer::OnSessionClonedArgs& args) {
              EXPECT_FALSE(args.success);
              EXPECT_THAT(args.error, HasSubstr("Tracing session not found"));
              clone_failed();
            }));
    ConsumerEndpoint::CloneSessionArgs args_f;
    args_f.unique_session_name = "my_unique_session_name";
    consumer3->endpoint()->CloneSession(args_f);
    task_runner.RunUntilCheckpoint("clone_failed");

    // But it should be possible to clone that by id.
    auto clone_success = task_runner.CreateCheckpoint("clone_success");
    EXPECT_CALL(*consumer3, OnSessionCloned(_))
        .WillOnce(
            Invoke([clone_success](const Consumer::OnSessionClonedArgs& args) {
              EXPECT_TRUE(args.success);
              clone_success();
            }));
    ConsumerEndpoint::CloneSessionArgs args_s;
    args_s.tsid = GetLastTracingSessionId(consumer3.get());
    consumer3->endpoint()->CloneSession(args_s);
    task_runner.RunUntilCheckpoint("clone_success");
  }
}

TEST_F(TracingServiceImplTest, InvalidBufferSizes) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_buffers()->set_size_kb(256);
  trace_config.add_buffers()->set_size_kb(4 * 1024 * 1024);
  auto* ds = trace_config.add_data_sources();
  auto* ds_config = ds->mutable_config();
  ds_config->set_name("data_source");
  consumer->EnableTracing(trace_config);

  std::string error;
  auto checkpoint = task_runner.CreateCheckpoint("tracing_disabled");
  EXPECT_CALL(*consumer, OnTracingDisabled(_))
      .WillOnce(DoAll(SaveArg<0>(&error), checkpoint));
  task_runner.RunUntilCheckpoint("tracing_disabled");
  EXPECT_THAT(error, HasSubstr("Invalid buffer sizes"));
}

TEST_F(TracingServiceImplTest, StringFiltering) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);  // Buf 0.
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  protozero::FilterBytecodeGenerator filt;
  // Message 0: root Trace proto.
  filt.AddNestedField(1 /* root trace.packet*/, 1);
  filt.EndMessage();
  // Message 1: TracePacket proto. Allow only the `for_testing` sub-field.
  filt.AddNestedField(protos::pbzero::TracePacket::kForTestingFieldNumber, 2);
  filt.EndMessage();
  // Message 2: TestEvent proto. Allow only the `str` sub-field as a striong.
  filt.AddFilterStringField(protos::pbzero::TestEvent::kStrFieldNumber);
  filt.EndMessage();
  trace_config.mutable_trace_filter()->set_bytecode_v2(filt.Serialize());

  auto* chain =
      trace_config.mutable_trace_filter()->mutable_string_filter_chain();
  auto* rule = chain->add_rules();
  rule->set_policy(
      protos::gen::TraceConfig::TraceFilter::SFP_ATRACE_MATCH_REDACT_GROUPS);
  rule->set_atrace_payload_starts_with("payload1");
  rule->set_regex_pattern(R"(B\|\d+\|pay(lo)ad1(\d*))");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");
  static constexpr size_t kNumTestPackets = 20;
  for (size_t i = 0; i < kNumTestPackets; i++) {
    auto tp = writer->NewTracePacket();
    std::string payload("B|1023|payload" + std::to_string(i));
    tp->set_for_testing()->set_str(payload.c_str(), payload.size());
  }

  auto flush_request = consumer->Flush();
  producer->ExpectFlush(writer.get());
  ASSERT_TRUE(flush_request.WaitForReply());

  const DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds_1");
  EXPECT_CALL(*producer, StopDataSource(id1));

  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("B|1023|payP6ad1")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("B|1023|payP6ad1P")))));
}

TEST_F(TracingServiceImplTest, StringFilteringAndCloneSession) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);  // Buf 0.
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  protozero::FilterBytecodeGenerator filt;
  // Message 0: root Trace proto.
  filt.AddNestedField(1 /* root trace.packet*/, 1);
  filt.EndMessage();
  // Message 1: TracePacket proto. Allow only the `for_testing` sub-field.
  filt.AddNestedField(protos::pbzero::TracePacket::kForTestingFieldNumber, 2);
  filt.EndMessage();
  // Message 2: TestEvent proto. Allow only the `str` sub-field as a string.
  filt.AddFilterStringField(protos::pbzero::TestEvent::kStrFieldNumber);
  filt.EndMessage();
  trace_config.mutable_trace_filter()->set_bytecode_v2(filt.Serialize());

  auto* chain =
      trace_config.mutable_trace_filter()->mutable_string_filter_chain();
  auto* rule = chain->add_rules();
  rule->set_policy(
      protos::gen::TraceConfig::TraceFilter::SFP_ATRACE_MATCH_REDACT_GROUPS);
  rule->set_atrace_payload_starts_with("payload");
  rule->set_regex_pattern(R"(B\|\d+\|pay(lo)ad(\d*))");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();

  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer = producer->CreateTraceWriter("ds_1");

  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("B|1023|payload");
  }

  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());

  auto clone_done = task_runner.CreateCheckpoint("clone_done");
  EXPECT_CALL(*consumer2, OnSessionCloned(_))
      .WillOnce(Invoke([clone_done](const Consumer::OnSessionClonedArgs&) {
        clone_done();
      }));
  consumer2->CloneSession(1);
  // CloneSession() will implicitly issue a flush. Linearize with that.
  producer->ExpectFlush(std::vector<TraceWriter*>{writer.get()});
  task_runner.RunUntilCheckpoint("clone_done");

  const DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("ds_1");
  EXPECT_CALL(*producer, StopDataSource(id1));

  consumer->DisableTracing();
  consumer->WaitForTracingDisabled();

  auto packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("B|1023|payP6ad")))));
  EXPECT_THAT(packets,
              Not(Contains(Property(&protos::gen::TracePacket::for_testing,
                                    Property(&protos::gen::TestEvent::str,
                                             Eq("B|1023|payload"))))));

  auto cloned_packets = consumer2->ReadBuffers();
  EXPECT_THAT(cloned_packets,
              Contains(Property(&protos::gen::TracePacket::for_testing,
                                Property(&protos::gen::TestEvent::str,
                                         Eq("B|1023|payP6ad")))));
  EXPECT_THAT(cloned_packets,
              Not(Contains(Property(&protos::gen::TracePacket::for_testing,
                                    Property(&protos::gen::TestEvent::str,
                                             Eq("B|1023|payload"))))));
}

// This is a regression test for https://b.corp.google.com/issues/307601836. The
// test covers the case of a consumer disconnecting while the tracing session is
// executing the final flush.
TEST_F(TracingServiceImplTest, ConsumerDisconnectionRacesFlushAndDisable) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  producer->RegisterDataSource("ds");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* trigger_config = trace_config.mutable_trigger_config();
  trigger_config->set_trigger_mode(TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_config->set_trigger_timeout_ms(100000);
  auto* trigger = trigger_config->add_triggers();
  trigger->set_name("trigger_name");
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds");
  producer->WaitForDataSourceStart("ds");

  auto writer1 = producer->CreateTraceWriter("ds");

  auto producer_flush_cb = [&](FlushRequestID flush_req_id,
                               const DataSourceInstanceID* /*id*/, size_t,
                               FlushFlags) {
    // Notify the tracing service that the flush is complete.
    producer->endpoint()->NotifyFlushComplete(flush_req_id);
    // Also disconnect the consumer (this terminates the tracing session). The
    // consumer disconnection is postponed with a PostTask(). The goal is to run
    // the lambda inside TracingServiceImpl::FlushAndDisableTracing() with an
    // empty `tracing_sessions_` map.
    task_runner.PostTask([&]() { consumer.reset(); });
  };
  EXPECT_CALL(*producer, Flush(_, _, _, _)).WillOnce(Invoke(producer_flush_cb));

  // Cause the tracing session to stop. Note that
  // TracingServiceImpl::FlushAndDisableTracing() is also called when
  // duration_ms expires, but in a test it's faster to use a trigger.
  producer->endpoint()->ActivateTriggers({"trigger_name"});
  producer->WaitForDataSourceStop("ds");

  task_runner.RunUntilIdle();
}

TEST_F(TracingServiceImplTest, RelayEndpointClockSync) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  auto relay_client = svc->ConnectRelayClient(
      std::make_pair<uint32_t, uint64_t>(/*base::MachineID=*/0x103, 1));

  uint32_t clock_id =
      static_cast<uint32_t>(protos::gen::BuiltinClock::BUILTIN_CLOCK_BOOTTIME);

  relay_client->SyncClocks(RelayEndpoint::SyncMode::PING,
                           /*client_clocks=*/{{clock_id, 100}},
                           /*host_clocks=*/{{clock_id, 1000}});
  relay_client->SyncClocks(RelayEndpoint::SyncMode::UPDATE,
                           /*client_clocks=*/{{clock_id, 300}},
                           /*host_clocks=*/{{clock_id, 1200}});

  producer->RegisterDataSource("ds");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds");
  producer->WaitForDataSourceStart("ds");

  auto writer1 = producer->CreateTraceWriter("ds");

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds");
  consumer->WaitForTracingDisabled();

  task_runner.RunUntilIdle();

  auto trace_packets = consumer->ReadBuffers();
  bool clock_sync_packet_seen = false;
  for (auto& packet : trace_packets) {
    if (!packet.has_remote_clock_sync())
      continue;
    clock_sync_packet_seen = true;

    auto& remote_clock_sync = packet.remote_clock_sync();
    ASSERT_EQ(remote_clock_sync.synced_clocks_size(), 2);

    auto& snapshots = remote_clock_sync.synced_clocks();
    ASSERT_TRUE(snapshots[0].has_client_clocks());
    auto* snapshot = &snapshots[0].client_clocks();
    ASSERT_EQ(snapshot->clocks_size(), 1);
    ASSERT_EQ(snapshot->clocks()[0].clock_id(), clock_id);
    ASSERT_EQ(snapshot->clocks()[0].timestamp(), 100u);

    snapshot = &snapshots[0].host_clocks();
    ASSERT_EQ(snapshot->clocks_size(), 1);
    ASSERT_EQ(snapshot->clocks()[0].clock_id(), clock_id);
    ASSERT_EQ(snapshot->clocks()[0].timestamp(), 1000u);

    snapshot = &snapshots[1].client_clocks();
    ASSERT_EQ(snapshot->clocks_size(), 1);
    ASSERT_EQ(snapshot->clocks()[0].clock_id(), clock_id);
    ASSERT_EQ(snapshot->clocks()[0].timestamp(), 300u);

    snapshot = &snapshots[1].host_clocks();
    ASSERT_EQ(snapshot->clocks_size(), 1);
    ASSERT_EQ(snapshot->clocks()[0].clock_id(), clock_id);
    ASSERT_EQ(snapshot->clocks()[0].timestamp(), 1200u);
  }
  ASSERT_TRUE(clock_sync_packet_seen);
}

TEST_F(TracingServiceImplTest, RelayEndpointDisconnect) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  auto relay_client = svc->ConnectRelayClient(
      std::make_pair<uint32_t, uint64_t>(/*base::MachineID=*/0x103, 1));
  uint32_t clock_id =
      static_cast<uint32_t>(protos::gen::BuiltinClock::BUILTIN_CLOCK_BOOTTIME);

  relay_client->SyncClocks(RelayEndpoint::SyncMode::PING,
                           /*client_clocks=*/{{clock_id, 100}},
                           /*host_clocks=*/{{clock_id, 1000}});
  relay_client->SyncClocks(RelayEndpoint::SyncMode::UPDATE,
                           /*client_clocks=*/{{clock_id, 300}},
                           /*host_clocks=*/{{clock_id, 1200}});

  relay_client->Disconnect();

  producer->RegisterDataSource("ds");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds");

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds");
  producer->WaitForDataSourceStart("ds");

  auto writer1 = producer->CreateTraceWriter("ds");

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds");
  consumer->WaitForTracingDisabled();

  task_runner.RunUntilIdle();

  auto trace_packets = consumer->ReadBuffers();
  bool clock_sync_packet_seen = false;
  for (auto& packet : trace_packets) {
    if (!packet.has_remote_clock_sync())
      continue;
    clock_sync_packet_seen = true;
  }
  ASSERT_FALSE(clock_sync_packet_seen);
}

TEST_F(TracingServiceImplTest, SessionSemaphoreMutexSingleSession) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);  // Buf 0.
  trace_config.add_session_semaphores()->set_name("mutex");

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  consumer->EnableTracing(trace_config);
  consumer->DisableTracing();
  consumer->WaitForTracingDisabledWithError(IsEmpty());
}

TEST_F(TracingServiceImplTest, SessionSemaphoreMutexMultipleSession) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);
  trace_config.add_session_semaphores()->set_name("mutex");

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  consumer->EnableTracing(trace_config);

  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());
  consumer2->EnableTracing(trace_config);
  consumer2->WaitForTracingDisabledWithError(LowerCase(HasSubstr("semaphore")));

  consumer->DisableTracing();
  consumer->WaitForTracingDisabledWithError(IsEmpty());
}

TEST_F(TracingServiceImplTest, SessionSemaphoreHigherCurrentFails) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);

  auto* session_semaphore = trace_config.add_session_semaphores();
  session_semaphore->set_name("diff_value_semaphore");
  session_semaphore->set_max_other_session_count(0);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  consumer->EnableTracing(trace_config);

  // The second consumer sets a higher count.
  session_semaphore->set_max_other_session_count(1);

  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());
  consumer2->EnableTracing(trace_config);
  consumer2->WaitForTracingDisabledWithError(LowerCase(HasSubstr("semaphore")));

  consumer->DisableTracing();
  consumer->WaitForTracingDisabledWithError(IsEmpty());
}

TEST_F(TracingServiceImplTest, SessionSemaphoreHigherPreviousFails) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);

  auto* session_semaphore = trace_config.add_session_semaphores();
  session_semaphore->set_name("diff_value_semaphore");
  session_semaphore->set_max_other_session_count(1);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  consumer->EnableTracing(trace_config);

  // The second consumer sets a lower count.
  session_semaphore->set_max_other_session_count(0);

  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());
  consumer2->EnableTracing(trace_config);
  consumer2->WaitForTracingDisabledWithError(LowerCase(HasSubstr("semaphore")));

  consumer->DisableTracing();
  consumer->WaitForTracingDisabledWithError(IsEmpty());
}

TEST_F(TracingServiceImplTest, SessionSemaphoreAllowedUpToLimit) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);

  auto* session_semaphore = trace_config.add_session_semaphores();
  session_semaphore->set_name("multi_semaphore");
  session_semaphore->set_max_other_session_count(3);

  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());
  consumer->EnableTracing(trace_config);

  std::unique_ptr<MockConsumer> consumer2 = CreateMockConsumer();
  consumer2->Connect(svc.get());
  consumer2->EnableTracing(trace_config);

  std::unique_ptr<MockConsumer> consumer3 = CreateMockConsumer();
  consumer3->Connect(svc.get());
  consumer3->EnableTracing(trace_config);

  std::unique_ptr<MockConsumer> consumer4 = CreateMockConsumer();
  consumer4->Connect(svc.get());
  consumer4->EnableTracing(trace_config);

  std::unique_ptr<MockConsumer> consumer5 = CreateMockConsumer();
  consumer5->Connect(svc.get());
  consumer5->EnableTracing(trace_config);
  consumer5->WaitForTracingDisabledWithError(LowerCase(HasSubstr("semaphore")));

  consumer4->DisableTracing();
  consumer4->WaitForTracingDisabledWithError(IsEmpty());

  consumer3->DisableTracing();
  consumer3->WaitForTracingDisabledWithError(IsEmpty());

  consumer2->DisableTracing();
  consumer2->WaitForTracingDisabledWithError(IsEmpty());

  consumer->DisableTracing();
  consumer->WaitForTracingDisabledWithError(IsEmpty());
}

TEST_F(TracingServiceImplTest, DetachAttach) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
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

  std::string on_detach_name = "on_detach";
  auto on_detach = task_runner.CreateCheckpoint(on_detach_name);
  EXPECT_CALL(*consumer, OnDetach(Eq(true))).WillOnce(Invoke(on_detach));

  consumer->Detach("mykey");

  task_runner.RunUntilCheckpoint(on_detach_name);

  consumer.reset();

  std::unique_ptr<TraceWriter> writer =
      producer->CreateTraceWriter("data_source");
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-1");
  }
  {
    auto tp = writer->NewTracePacket();
    tp->set_for_testing()->set_str("payload-2");
  }

  writer->Flush();
  writer.reset();

  consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TraceConfig attached_config;
  std::string on_attach_name = "on_attach";
  auto on_attach = task_runner.CreateCheckpoint(on_attach_name);
  EXPECT_CALL(*consumer, OnAttach(Eq(true), _))
      .WillOnce(Invoke([&](bool, const TraceConfig& cfg) {
        attached_config = cfg;
        on_attach();
      }));

  consumer->Attach("mykey");

  task_runner.RunUntilCheckpoint(on_attach_name);

  EXPECT_EQ(attached_config, trace_config);

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source");
  consumer->WaitForTracingDisabled();

  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(packets, Not(IsEmpty()));
  EXPECT_THAT(
      packets,
      Each(Property(&protos::gen::TracePacket::has_compressed_packets, false)));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload-1")))));
  EXPECT_THAT(packets, Contains(Property(&protos::gen::TracePacket::for_testing,
                                         Property(&protos::gen::TestEvent::str,
                                                  Eq("payload-2")))));
}

TEST_F(TracingServiceImplTest, DetachDurationTimeoutFreeBuffers) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("data_source");
  trace_config.set_duration_ms(1);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(100000);
  auto pipe_pair = base::Pipe::Create();
  consumer->EnableTracing(trace_config, std::move(pipe_pair.wr));

  std::string on_detach_name = "on_detach";
  auto on_detach = task_runner.CreateCheckpoint(on_detach_name);
  EXPECT_CALL(*consumer, OnDetach(Eq(true))).WillOnce(Invoke(on_detach));

  consumer->Detach("mykey");

  task_runner.RunUntilCheckpoint(on_detach_name);

  std::string file_closed_name = "file_closed";
  auto file_closed = task_runner.CreateCheckpoint(file_closed_name);
  task_runner.AddFileDescriptorWatch(*pipe_pair.rd, [&] {
    char buf[1024];
    if (base::Read(*pipe_pair.rd, buf, sizeof(buf)) <= 0) {
      file_closed();
    }
  });
  task_runner.RunUntilCheckpoint(file_closed_name);

  // Disabled and detached tracing sessions are automatically deleted:
  // reattaching fails.
  std::string on_attach_name = "on_attach";
  auto on_attach = task_runner.CreateCheckpoint(on_attach_name);
  EXPECT_CALL(*consumer, OnAttach(Eq(false), _))
      .WillOnce(InvokeWithoutArgs(on_attach));
  consumer->Attach("mykey");
  task_runner.RunUntilCheckpoint(on_attach_name);
}

TEST_F(TracingServiceImplTest, SlowStartingDataSources) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer");
  producer->RegisterDataSource("data_source1", /*ack_stop=*/false,
                               /*ack_start=*/true);
  producer->RegisterDataSource("data_source2", /*ack_stop=*/false,
                               /*ack_start=*/true);
  producer->RegisterDataSource("data_source3", /*ack_stop=*/false,
                               /*ack_start=*/true);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("data_source1");
  trace_config.add_data_sources()->mutable_config()->set_name("data_source2");
  trace_config.add_data_sources()->mutable_config()->set_name("data_source3");
  consumer->EnableTracing(trace_config);

  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("data_source1");
  producer->WaitForDataSourceSetup("data_source2");
  producer->WaitForDataSourceSetup("data_source3");

  producer->WaitForDataSourceStart("data_source1");
  producer->WaitForDataSourceStart("data_source2");
  producer->WaitForDataSourceStart("data_source3");

  DataSourceInstanceID id1 = producer->GetDataSourceInstanceId("data_source1");
  DataSourceInstanceID id3 = producer->GetDataSourceInstanceId("data_source3");

  producer->endpoint()->NotifyDataSourceStarted(id1);
  producer->endpoint()->NotifyDataSourceStarted(id3);

  // This matches kAllDataSourceStartedTimeout.
  AdvanceTimeAndRunUntilIdle(20000);

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("data_source1");
  producer->WaitForDataSourceStop("data_source2");
  producer->WaitForDataSourceStop("data_source3");
  consumer->WaitForTracingDisabled();

  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      Contains(Property(
          &protos::gen::TracePacket::service_event,
          Property(
              &protos::gen::TracingServiceEvent::slow_starting_data_sources,
              Property(
                  &protos::gen::TracingServiceEvent::DataSources::data_source,
                  ElementsAre(
                      Property(&protos::gen::TracingServiceEvent::DataSources::
                                   DataSource::data_source_name,
                               "data_source2")))))));
}

TEST_F(TracingServiceImplTest, FlushTimeoutEventsEmitted) {
  std::unique_ptr<MockConsumer> consumer = CreateMockConsumer();
  consumer->Connect(svc.get());

  std::unique_ptr<MockProducer> producer = CreateMockProducer();
  producer->Connect(svc.get(), "mock_producer1");
  producer->RegisterDataSource("ds_1");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);  // Buf 0.
  auto* ds_cfg = trace_config.add_data_sources()->mutable_config();
  ds_cfg->set_name("ds_1");
  ds_cfg->set_target_buffer(0);

  consumer->EnableTracing(trace_config);
  producer->WaitForTracingSetup();
  producer->WaitForDataSourceSetup("ds_1");
  producer->WaitForDataSourceStart("ds_1");

  std::unique_ptr<TraceWriter> writer1 = producer->CreateTraceWriter("ds_1");

  // Do not reply to Flush.
  std::string producer_flush1_checkpoint_name = "producer_flush1_requested";
  auto flush1_requested =
      task_runner.CreateCheckpoint(producer_flush1_checkpoint_name);
  EXPECT_CALL(*producer, Flush).WillOnce(Invoke(flush1_requested));
  consumer->Flush(5000, FlushFlags(FlushFlags::Initiator::kTraced,
                                   FlushFlags::Reason::kTraceStop));

  task_runner.RunUntilCheckpoint(producer_flush1_checkpoint_name);

  AdvanceTimeAndRunUntilIdle(5000);

  // ReadBuffers returns a last_flush_slow_data_source event.
  std::vector<protos::gen::TracePacket> packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      Contains(Property(
          &protos::gen::TracePacket::service_event,
          Property(
              &protos::gen::TracingServiceEvent::last_flush_slow_data_sources,
              Property(
                  &protos::gen::TracingServiceEvent::DataSources::data_source,
                  ElementsAre(
                      Property(&protos::gen::TracingServiceEvent::DataSources::
                                   DataSource::data_source_name,
                               "ds_1")))))));

  // Reply to Flush.
  std::string producer_flush2_checkpoint_name = "producer_flush2_requested";
  auto flush2_requested =
      task_runner.CreateCheckpoint(producer_flush2_checkpoint_name);
  FlushRequestID flush2_req_id;
  EXPECT_CALL(*producer, Flush(_, _, _, _))
      .WillOnce([&](FlushRequestID req_id, const DataSourceInstanceID*, size_t,
                    FlushFlags) {
        flush2_req_id = req_id;
        flush2_requested();
      });
  consumer->Flush(5000, FlushFlags(FlushFlags::Initiator::kTraced,
                                   FlushFlags::Reason::kTraceStop));

  task_runner.RunUntilCheckpoint(producer_flush2_checkpoint_name);

  producer->endpoint()->NotifyFlushComplete(flush2_req_id);

  AdvanceTimeAndRunUntilIdle(5000);

  // ReadBuffers returns a last_flush_slow_data_source event.
  packets = consumer->ReadBuffers();
  EXPECT_THAT(
      packets,
      Not(Contains(Property(&protos::gen::TracePacket::service_event,
                            Property(&protos::gen::TracingServiceEvent::
                                         has_last_flush_slow_data_sources,
                                     Eq(true))))));

  consumer->DisableTracing();
  producer->WaitForDataSourceStop("ds_1");
  consumer->WaitForTracingDisabled();
}

}  // namespace

}  // namespace perfetto
