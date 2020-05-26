/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <fcntl.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <vector>

#include "perfetto/tracing.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.pb.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/log_message.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "test/gtest_and_gmock.h"

// Deliberately not pulling any non-public perfetto header to spot accidental
// header public -> non-public dependency while building this file.

// These two are the only headers allowed here, see comments in
// api_test_support.h.
#include "src/tracing/test/api_test_support.h"
#include "src/tracing/test/tracing_module.h"

#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"

// Trace categories used in the tests.
PERFETTO_DEFINE_CATEGORIES(PERFETTO_CATEGORY(test),
                           PERFETTO_CATEGORY(foo),
                           PERFETTO_CATEGORY(bar));
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

// For testing interning of complex objects.
using SourceLocation = std::tuple<const char* /* file_name */,
                                  const char* /* function_name */,
                                  uint32_t /* line_number */>;

namespace std {
template <>
struct hash<SourceLocation> {
  size_t operator()(const SourceLocation& value) const {
    auto hasher = hash<size_t>();
    return hasher(reinterpret_cast<size_t>(get<0>(value))) ^
           hasher(reinterpret_cast<size_t>(get<1>(value))) ^
           hasher(get<2>(value));
  }
};
}  // namespace std

namespace {

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::NiceMock;
using ::testing::Not;
using ::testing::Property;
using ::testing::StrEq;

// ------------------------------
// Declarations of helper classes
// ------------------------------
static constexpr auto kWaitEventTimeout = std::chrono::seconds(5);

struct WaitableTestEvent {
  std::mutex mutex_;
  std::condition_variable cv_;
  bool notified_ = false;

  bool notified() {
    std::unique_lock<std::mutex> lock(mutex_);
    return notified_;
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, kWaitEventTimeout, [this] { return notified_; })) {
      fprintf(stderr, "Timed out while waiting for event\n");
      abort();
    }
  }

  void Notify() {
    std::unique_lock<std::mutex> lock(mutex_);
    notified_ = true;
    cv_.notify_one();
  }
};

class MockDataSource;

// We can't easily use gmock here because instances of data sources are lazily
// created by the service and are not owned by the test fixture.
struct TestDataSourceHandle {
  WaitableTestEvent on_create;
  WaitableTestEvent on_setup;
  WaitableTestEvent on_start;
  WaitableTestEvent on_stop;
  MockDataSource* instance;
  perfetto::DataSourceConfig config;
  bool handle_stop_asynchronously = false;
  std::function<void()> on_start_callback;
  std::function<void()> on_stop_callback;
  std::function<void()> async_stop_closure;
};

class MockDataSource : public perfetto::DataSource<MockDataSource> {
 public:
  void OnSetup(const SetupArgs&) override;
  void OnStart(const StartArgs&) override;
  void OnStop(const StopArgs&) override;
  TestDataSourceHandle* handle_ = nullptr;
};

class MockDataSource2 : public perfetto::DataSource<MockDataSource2> {
 public:
  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// Used to verify that track event data sources in different namespaces register
// themselves correctly in the muxer.
class MockTracingMuxer : public perfetto::internal::TracingMuxer {
 public:
  struct DataSource {
    const perfetto::DataSourceDescriptor dsd;
    perfetto::internal::DataSourceStaticState* static_state;
  };

  MockTracingMuxer() : TracingMuxer(nullptr), prev_instance_(instance_) {
    instance_ = this;
  }
  ~MockTracingMuxer() override { instance_ = prev_instance_; }

  bool RegisterDataSource(
      const perfetto::DataSourceDescriptor& dsd,
      DataSourceFactory,
      perfetto::internal::DataSourceStaticState* static_state) override {
    data_sources.emplace_back(DataSource{dsd, static_state});
    return true;
  }

  std::unique_ptr<perfetto::TraceWriterBase> CreateTraceWriter(
      perfetto::internal::DataSourceState*,
      perfetto::BufferExhaustedPolicy) override {
    return nullptr;
  }

  void DestroyStoppedTraceWritersForCurrentThread() override {}

  std::vector<DataSource> data_sources;

 private:
  TracingMuxer* prev_instance_;
};

struct TestIncrementalState {
  TestIncrementalState() { constructed = true; }
  // Note: a virtual destructor is not required for incremental state.
  ~TestIncrementalState() { destroyed = true; }

  int count = 100;
  static bool constructed;
  static bool destroyed;
};

bool TestIncrementalState::constructed;
bool TestIncrementalState::destroyed;

struct TestIncrementalDataSourceTraits
    : public perfetto::DefaultDataSourceTraits {
  using IncrementalStateType = TestIncrementalState;
};

class TestIncrementalDataSource
    : public perfetto::DataSource<TestIncrementalDataSource,
                                  TestIncrementalDataSourceTraits> {
 public:
  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// A convenience wrapper around TracingSession that allows to do block on
//
struct TestTracingSessionHandle {
  perfetto::TracingSession* get() { return session.get(); }
  std::unique_ptr<perfetto::TracingSession> session;
  WaitableTestEvent on_stop;
};

// -------------------------
// Declaration of test class
// -------------------------
class PerfettoApiTest : public ::testing::Test {
 public:
  static PerfettoApiTest* instance;

  void SetUp() override {
    instance = this;
    // Perfetto can only be initialized once in a process.
    static bool was_initialized;
    if (!was_initialized) {
      perfetto::TracingInitArgs args;
      args.backends = perfetto::kInProcessBackend;
      perfetto::Tracing::Initialize(args);
      was_initialized = true;
      RegisterDataSource<MockDataSource>("my_data_source");
      perfetto::TrackEvent::Register();
    }
    // Make sure our data source always has a valid handle.
    data_sources_["my_data_source"];
  }

  void TearDown() override { instance = nullptr; }

  template <typename DataSourceType>
  TestDataSourceHandle* RegisterDataSource(std::string name) {
    EXPECT_EQ(data_sources_.count(name), 0u);
    TestDataSourceHandle* handle = &data_sources_[name];
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name(name);
    DataSourceType::Register(dsd);
    return handle;
  }

  TestTracingSessionHandle* NewTrace(const perfetto::TraceConfig& cfg,
                                     int fd = -1) {
    sessions_.emplace_back();
    TestTracingSessionHandle* handle = &sessions_.back();
    handle->session =
        perfetto::Tracing::NewTrace(perfetto::BackendType::kInProcessBackend);
    handle->session->SetOnStopCallback([handle] { handle->on_stop.Notify(); });
    handle->session->Setup(cfg, fd);
    return handle;
  }

  std::vector<std::string> ReadLogMessagesFromTrace(
      perfetto::TracingSession* tracing_session) {
    std::vector<char> raw_trace = tracing_session->ReadTraceBlocking();
    EXPECT_GE(raw_trace.size(), 0u);

    // Read back the trace, maintaining interning tables as we go.
    std::vector<std::string> log_messages;
    std::map<uint64_t, std::string> log_message_bodies;
    std::map<uint64_t, perfetto::protos::SourceLocation> source_locations;
    perfetto::protos::Trace parsed_trace;
    EXPECT_TRUE(
        parsed_trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));

    for (const auto& packet : parsed_trace.packet()) {
      if (!packet.has_track_event())
        continue;

      if (packet.has_interned_data()) {
        const auto& interned_data = packet.interned_data();
        for (const auto& it : interned_data.log_message_body()) {
          EXPECT_GE(it.iid(), 1u);
          EXPECT_EQ(log_message_bodies.find(it.iid()),
                    log_message_bodies.end());
          log_message_bodies[it.iid()] = it.body();
        }
        for (const auto& it : interned_data.source_locations()) {
          EXPECT_GE(it.iid(), 1u);
          EXPECT_EQ(source_locations.find(it.iid()), source_locations.end());
          source_locations[it.iid()] = it;
        }
      }
      const auto& track_event = packet.track_event();
      if (track_event.type() != perfetto::protos::TrackEvent::TYPE_SLICE_BEGIN)
        continue;

      EXPECT_TRUE(track_event.has_log_message());
      const auto& log = track_event.log_message();
      if (log.source_location_iid()) {
        std::stringstream msg;
        const auto& source_location =
            source_locations[log.source_location_iid()];
        msg << source_location.function_name() << "("
            << source_location.file_name() << ":"
            << source_location.line_number()
            << "): " << log_message_bodies[log.body_iid()];
        log_messages.emplace_back(msg.str());
      } else {
        log_messages.emplace_back(log_message_bodies[log.body_iid()]);
      }
    }
    return log_messages;
  }

  std::vector<std::string> ReadSlicesFromTrace(
      perfetto::TracingSession* tracing_session) {
    std::vector<char> raw_trace = tracing_session->ReadTraceBlocking();
    EXPECT_GE(raw_trace.size(), 0u);

    // Read back the trace, maintaining interning tables as we go.
    std::vector<std::string> slices;
    std::map<uint64_t, std::string> categories;
    std::map<uint64_t, std::string> event_names;
    std::map<uint64_t, std::string> debug_annotation_names;
    perfetto::protos::Trace parsed_trace;
    EXPECT_TRUE(
        parsed_trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));

    bool incremental_state_was_cleared = false;
    uint32_t sequence_id = 0;
    for (const auto& packet : parsed_trace.packet()) {
      if (packet.incremental_state_cleared()) {
        incremental_state_was_cleared = true;
        categories.clear();
        event_names.clear();
        debug_annotation_names.clear();
      }

      if (!packet.has_track_event())
        continue;

      // Make sure we only see track events on one sequence.
      if (packet.trusted_packet_sequence_id()) {
        if (!sequence_id)
          sequence_id = packet.trusted_packet_sequence_id();
        EXPECT_EQ(sequence_id, packet.trusted_packet_sequence_id());
      }

      // Update incremental state.
      if (packet.has_interned_data()) {
        const auto& interned_data = packet.interned_data();
        for (const auto& it : interned_data.event_categories()) {
          EXPECT_EQ(categories.find(it.iid()), categories.end());
          categories[it.iid()] = it.name();
        }
        for (const auto& it : interned_data.event_names()) {
          EXPECT_EQ(event_names.find(it.iid()), event_names.end());
          event_names[it.iid()] = it.name();
        }
        for (const auto& it : interned_data.debug_annotation_names()) {
          EXPECT_EQ(debug_annotation_names.find(it.iid()),
                    debug_annotation_names.end());
          debug_annotation_names[it.iid()] = it.name();
        }
      }
      const auto& track_event = packet.track_event();
      std::string slice;
      switch (track_event.type()) {
        case perfetto::protos::TrackEvent::TYPE_SLICE_BEGIN:
          slice += "B";
          break;
        case perfetto::protos::TrackEvent::TYPE_SLICE_END:
          slice += "E";
          break;
        case perfetto::protos::TrackEvent::TYPE_INSTANT:
          slice += "I";
          break;
        default:
        case perfetto::protos::TrackEvent::TYPE_UNSPECIFIED:
          EXPECT_FALSE(track_event.type());
      }
      slice += ":" + categories[track_event.category_iids().Get(0)] + "." +
               event_names[track_event.name_iid()];

      if (track_event.debug_annotations_size()) {
        slice += "(";
        bool first_annotation = true;
        for (const auto& it : track_event.debug_annotations()) {
          if (!first_annotation) {
            slice += ",";
          }
          slice += debug_annotation_names[it.name_iid()] + "=";
          std::stringstream value;
          if (it.has_bool_value()) {
            value << "(bool)" << it.bool_value();
          } else if (it.has_uint_value()) {
            value << "(uint)" << it.uint_value();
          } else if (it.has_int_value()) {
            value << "(int)" << it.int_value();
          } else if (it.has_double_value()) {
            value << "(double)" << it.double_value();
          } else if (it.has_string_value()) {
            value << "(string)" << it.string_value();
          } else if (it.has_pointer_value()) {
            value << "(pointer)" << std::hex << it.pointer_value();
          } else if (it.has_legacy_json_value()) {
            value << "(json)" << it.legacy_json_value();
          } else if (it.has_nested_value()) {
            value << "(nested)" << it.nested_value().string_value();
          }
          slice += value.str();
          first_annotation = false;
        }
        slice += ")";
      }

      slices.push_back(slice);
    }
    EXPECT_TRUE(incremental_state_was_cleared);
    return slices;
  }

  std::map<std::string, TestDataSourceHandle> data_sources_;
  std::list<TestTracingSessionHandle> sessions_;  // Needs stable pointers.
};

// ---------------------------------------------
// Definitions for non-inlineable helper methods
// ---------------------------------------------
PerfettoApiTest* PerfettoApiTest::instance;

void MockDataSource::OnSetup(const SetupArgs& args) {
  EXPECT_EQ(handle_, nullptr);
  auto it = PerfettoApiTest::instance->data_sources_.find(args.config->name());

  // We should not see an OnSetup for a data source that we didn't register
  // before via PerfettoApiTest::RegisterDataSource().
  EXPECT_NE(it, PerfettoApiTest::instance->data_sources_.end());
  handle_ = &it->second;
  handle_->config = *args.config;  // Deliberate copy.
  handle_->on_setup.Notify();
}

void MockDataSource::OnStart(const StartArgs&) {
  EXPECT_NE(handle_, nullptr);
  if (handle_->on_start_callback)
    handle_->on_start_callback();
  handle_->on_start.Notify();
}

void MockDataSource::OnStop(const StopArgs& args) {
  EXPECT_NE(handle_, nullptr);
  if (handle_->handle_stop_asynchronously)
    handle_->async_stop_closure = args.HandleStopAsynchronously();
  if (handle_->on_stop_callback)
    handle_->on_stop_callback();
  handle_->on_stop.Notify();
}

// -------------
// Test fixtures
// -------------

TEST_F(PerfettoApiTest, TrackEventStartStopAndDestroy) {
  // This test used to cause a use after free as the tracing session got
  // destroyed. It needed to be run approximately 2000 times to catch it so test
  // with --gtest_repeat=3000 (less if running under GDB).

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");
  // Create five new trace sessions.
  std::vector<std::unique_ptr<perfetto::TracingSession>> sessions;
  for (size_t i = 0; i < 5; ++i) {
    sessions.push_back(
        perfetto::Tracing::NewTrace(perfetto::BackendType::kInProcessBackend));
    sessions[i]->Setup(cfg);
    sessions[i]->Start();
    sessions[i]->Stop();
  }
}

TEST_F(PerfettoApiTest, TrackEventStartStopAndStopBlocking) {
  // This test used to cause a deadlock (due to StopBlocking() after the session
  // already stopped). This usually occurred within 1 or 2 runs of the test so
  // use --gtest_repeat=10

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");
  // Create five new trace sessions.
  std::vector<std::unique_ptr<perfetto::TracingSession>> sessions;
  for (size_t i = 0; i < 5; ++i) {
    sessions.push_back(
        perfetto::Tracing::NewTrace(perfetto::BackendType::kInProcessBackend));
    sessions[i]->Setup(cfg);
    sessions[i]->Start();
    sessions[i]->Stop();
  }
  for (auto& session : sessions) {
    session->StopBlocking();
  }
}

TEST_F(PerfettoApiTest, TrackEvent) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // Emit one complete track event.
  TRACE_EVENT_BEGIN("test", "TestEvent");
  TRACE_EVENT_END("test");
  perfetto::TrackEvent::Flush();

  tracing_session->on_stop.Wait();
  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  ASSERT_GE(raw_trace.size(), 0u);

  // Read back the trace, maintaining interning tables as we go.
  perfetto::protos::Trace trace;
  std::map<uint64_t, std::string> categories;
  std::map<uint64_t, std::string> event_names;
  ASSERT_TRUE(trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));

  bool incremental_state_was_cleared = false;
  bool begin_found = false;
  bool end_found = false;
  bool process_descriptor_found = false;
  bool thread_descriptor_found = false;
  auto now = perfetto::test::GetTraceTimeNs();
  uint32_t sequence_id = 0;
  int32_t cur_pid = perfetto::test::GetCurrentProcessId();
  for (const auto& packet : trace.packet()) {
    if (packet.has_process_descriptor()) {
      EXPECT_FALSE(process_descriptor_found);
      const auto& pd = packet.process_descriptor();
      EXPECT_EQ(cur_pid, pd.pid());
      process_descriptor_found = true;
    }
    if (packet.has_thread_descriptor()) {
      EXPECT_FALSE(thread_descriptor_found);
      const auto& td = packet.thread_descriptor();
      EXPECT_EQ(cur_pid, td.pid());
      EXPECT_NE(0, td.tid());
      thread_descriptor_found = true;
    }
    if (packet.incremental_state_cleared()) {
      EXPECT_TRUE(packet.has_trace_packet_defaults());
      incremental_state_was_cleared = true;
      categories.clear();
      event_names.clear();
    }

    if (!packet.has_track_event())
      continue;
    const auto& track_event = packet.track_event();

    // Make sure we only see track events on one sequence.
    if (packet.trusted_packet_sequence_id()) {
      if (!sequence_id)
        sequence_id = packet.trusted_packet_sequence_id();
      EXPECT_EQ(sequence_id, packet.trusted_packet_sequence_id());
    }

    // Update incremental state.
    if (packet.has_interned_data()) {
      const auto& interned_data = packet.interned_data();
      for (const auto& it : interned_data.event_categories()) {
        EXPECT_EQ(categories.find(it.iid()), categories.end());
        categories[it.iid()] = it.name();
      }
      for (const auto& it : interned_data.event_names()) {
        EXPECT_EQ(event_names.find(it.iid()), event_names.end());
        event_names[it.iid()] = it.name();
      }
    }

    EXPECT_GT(packet.timestamp(), 0u);
    EXPECT_LE(packet.timestamp(), now);
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    EXPECT_FALSE(packet.has_timestamp_clock_id());
#else
    constexpr auto kClockMonotonic =
        perfetto::protos::pbzero::ClockSnapshot::Clock::MONOTONIC;
    EXPECT_EQ(packet.timestamp_clock_id(), kClockMonotonic);
#endif
    EXPECT_EQ(track_event.category_iids().size(), 1);
    EXPECT_GE(track_event.category_iids().Get(0), 1u);

    if (track_event.type() == perfetto::protos::TrackEvent::TYPE_SLICE_BEGIN) {
      EXPECT_FALSE(begin_found);
      EXPECT_EQ("test", categories[track_event.category_iids().Get(0)]);
      EXPECT_EQ("TestEvent", event_names[track_event.name_iid()]);
      begin_found = true;
    } else if (track_event.type() ==
               perfetto::protos::TrackEvent::TYPE_SLICE_END) {
      EXPECT_FALSE(end_found);
      EXPECT_EQ(0u, track_event.name_iid());
      EXPECT_EQ("test", categories[track_event.category_iids().Get(0)]);
      end_found = true;
    }
  }
  EXPECT_TRUE(incremental_state_was_cleared);
  EXPECT_TRUE(process_descriptor_found);
  EXPECT_TRUE(thread_descriptor_found);
  EXPECT_TRUE(begin_found);
  EXPECT_TRUE(end_found);
}

TEST_F(PerfettoApiTest, TrackEventCategories) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("bar");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // Emit some track events.
  TRACE_EVENT_BEGIN("foo", "NotEnabled");
  TRACE_EVENT_END("foo");
  TRACE_EVENT_BEGIN("bar", "Enabled");
  TRACE_EVENT_END("bar");

  tracing_session->get()->StopBlocking();
  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  std::string trace(raw_trace.data(), raw_trace.size());
  // TODO(skyostil): Come up with a nicer way to verify trace contents.
  EXPECT_THAT(trace, HasSubstr("Enabled"));
  EXPECT_THAT(trace, Not(HasSubstr("NotEnabled")));
}

TEST_F(PerfettoApiTest, TrackEventRegistrationWithModule) {
  MockTracingMuxer muxer;

  // Each track event namespace registers its own data source.
  perfetto::TrackEvent::Register();
  EXPECT_EQ(1u, muxer.data_sources.size());

  tracing_module::InitializeCategories();
  EXPECT_EQ(2u, muxer.data_sources.size());

  // Both data sources have the same name but distinct static data (i.e.,
  // individual instance states).
  EXPECT_EQ("track_event", muxer.data_sources[0].dsd.name());
  EXPECT_EQ("track_event", muxer.data_sources[1].dsd.name());
  EXPECT_NE(muxer.data_sources[0].static_state,
            muxer.data_sources[1].static_state);
}

TEST_F(PerfettoApiTest, TrackEventSharedIncrementalState) {
  tracing_module::InitializeCategories();

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  perfetto::internal::TrackEventIncrementalState* main_state = nullptr;
  perfetto::TrackEvent::Trace(
      [&main_state](perfetto::TrackEvent::TraceContext ctx) {
        main_state = ctx.GetIncrementalState();
      });
  perfetto::internal::TrackEventIncrementalState* module_state =
      tracing_module::GetIncrementalState();

  // Both track event data sources should use the same incremental state (thanks
  // to sharing TLS).
  EXPECT_NE(nullptr, main_state);
  EXPECT_EQ(main_state, module_state);
  tracing_session->get()->StopBlocking();
}

TEST_F(PerfettoApiTest, TrackEventCategoriesWithModule) {
  // Check that categories defined in two different category registries are
  // enabled and disabled correctly.
  tracing_module::InitializeCategories();

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");

  // Only the "foo" category is enabled. It also exists both locally and in the
  // existing module.
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // Emit some track events locally and from the test module.
  TRACE_EVENT_BEGIN("foo", "FooEventFromMain");
  TRACE_EVENT_END("foo");
  tracing_module::EmitTrackEvents();
  tracing_module::EmitTrackEvents2();
  TRACE_EVENT_BEGIN("bar", "DisabledEventFromMain");
  TRACE_EVENT_END("bar");

  tracing_session->get()->StopBlocking();
  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  std::string trace(raw_trace.data(), raw_trace.size());
  EXPECT_THAT(trace, HasSubstr("FooEventFromMain"));
  EXPECT_THAT(trace, Not(HasSubstr("DisabledEventFromMain")));
  EXPECT_THAT(trace, HasSubstr("FooEventFromModule"));
  EXPECT_THAT(trace, Not(HasSubstr("DisabledEventFromModule")));
  EXPECT_THAT(trace, HasSubstr("FooEventFromModule2"));
  EXPECT_THAT(trace, Not(HasSubstr("DisabledEventFromModule2")));

  perfetto::protos::Trace parsed_trace;
  ASSERT_TRUE(
      parsed_trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));

  uint32_t sequence_id = 0;
  for (const auto& packet : parsed_trace.packet()) {
    if (!packet.has_track_event())
      continue;

    // Make sure we only see track events on one sequence. This means all track
    // event modules are sharing the same trace writer (by using the same TLS
    // index).
    if (packet.trusted_packet_sequence_id()) {
      if (!sequence_id)
        sequence_id = packet.trusted_packet_sequence_id();
      EXPECT_EQ(sequence_id, packet.trusted_packet_sequence_id());
    }
  }
}

TEST_F(PerfettoApiTest, TrackEventConcurrentSessions) {
  // Check that categories that are enabled and disabled in two parallel tracing
  // sessions don't interfere.

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");

  // Session #1 enables the "foo" category.
  ds_cfg->set_legacy_config("foo");
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // Session #2 enables the "bar" category.
  ds_cfg->set_legacy_config("bar");
  auto* tracing_session2 = NewTrace(cfg);
  tracing_session2->get()->StartBlocking();

  // Emit some track events under both categories.
  TRACE_EVENT_BEGIN("foo", "Session1_First");
  TRACE_EVENT_END("foo");
  TRACE_EVENT_BEGIN("bar", "Session2_First");
  TRACE_EVENT_END("bar");

  tracing_session->get()->StopBlocking();
  TRACE_EVENT_BEGIN("foo", "Session1_Second");
  TRACE_EVENT_END("foo");
  TRACE_EVENT_BEGIN("bar", "Session2_Second");
  TRACE_EVENT_END("bar");

  tracing_session2->get()->StopBlocking();
  TRACE_EVENT_BEGIN("foo", "Session1_Third");
  TRACE_EVENT_END("foo");
  TRACE_EVENT_BEGIN("bar", "Session2_Third");
  TRACE_EVENT_END("bar");

  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  std::string trace(raw_trace.data(), raw_trace.size());
  EXPECT_THAT(trace, HasSubstr("Session1_First"));
  EXPECT_THAT(trace, Not(HasSubstr("Session1_Second")));
  EXPECT_THAT(trace, Not(HasSubstr("Session1_Third")));
  EXPECT_THAT(trace, Not(HasSubstr("Session2_First")));
  EXPECT_THAT(trace, Not(HasSubstr("Session2_Second")));
  EXPECT_THAT(trace, Not(HasSubstr("Session2_Third")));

  std::vector<char> raw_trace2 = tracing_session2->get()->ReadTraceBlocking();
  std::string trace2(raw_trace2.data(), raw_trace2.size());
  EXPECT_THAT(trace2, Not(HasSubstr("Session1_First")));
  EXPECT_THAT(trace2, Not(HasSubstr("Session1_Second")));
  EXPECT_THAT(trace2, Not(HasSubstr("Session1_Third")));
  EXPECT_THAT(trace2, HasSubstr("Session2_First"));
  EXPECT_THAT(trace2, HasSubstr("Session2_Second"));
  EXPECT_THAT(trace2, Not(HasSubstr("Session2_Third")));
}

TEST_F(PerfettoApiTest, TrackEventTypedArgs) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  auto random_value = random();
  TRACE_EVENT_BEGIN("foo", "EventWithTypedArg",
                    [random_value](perfetto::EventContext ctx) {
                      auto* log = ctx.event()->set_log_message();
                      log->set_source_location_iid(1);
                      log->set_body_iid(2);
                      auto* dbg = ctx.event()->add_debug_annotations();
                      dbg->set_name("random");
                      dbg->set_int_value(random_value);
                    });
  TRACE_EVENT_END("foo");

  tracing_session->get()->StopBlocking();
  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  std::string trace(raw_trace.data(), raw_trace.size());

  perfetto::protos::Trace parsed_trace;
  ASSERT_TRUE(
      parsed_trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));

  bool found_args = false;
  for (const auto& packet : parsed_trace.packet()) {
    if (!packet.has_track_event())
      continue;
    const auto& track_event = packet.track_event();
    if (track_event.type() != perfetto::protos::TrackEvent::TYPE_SLICE_BEGIN)
      continue;

    EXPECT_TRUE(track_event.has_log_message());
    const auto& log = track_event.log_message();
    EXPECT_EQ(1u, log.source_location_iid());
    EXPECT_EQ(2u, log.body_iid());

    const auto& dbg = track_event.debug_annotations().Get(0);
    EXPECT_EQ("random", dbg.name());
    EXPECT_EQ(random_value, dbg.int_value());

    found_args = true;
  }
  EXPECT_TRUE(found_args);
}

struct InternedLogMessageBody
    : public perfetto::TrackEventInternedDataIndex<
          InternedLogMessageBody,
          perfetto::protos::pbzero::InternedData::kLogMessageBodyFieldNumber,
          std::string> {
  static void Add(perfetto::protos::pbzero::InternedData* interned_data,
                  size_t iid,
                  const std::string& value) {
    auto l = interned_data->add_log_message_body();
    l->set_iid(iid);
    l->set_body(value.data(), value.size());
    commit_count++;
  }

  static int commit_count;
};

int InternedLogMessageBody::commit_count = 0;

TEST_F(PerfettoApiTest, TrackEventTypedArgsWithInterning) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  std::stringstream large_message;
  for (size_t i = 0; i < 512; i++)
    large_message << i << ". Something wicked this way comes. ";

  size_t body_iid;
  InternedLogMessageBody::commit_count = 0;
  TRACE_EVENT_BEGIN("foo", "EventWithState", [&](perfetto::EventContext ctx) {
    EXPECT_EQ(0, InternedLogMessageBody::commit_count);
    body_iid = InternedLogMessageBody::Get(&ctx, "Alas, poor Yorick!");
    auto log = ctx.event()->set_log_message();
    log->set_body_iid(body_iid);
    EXPECT_EQ(1, InternedLogMessageBody::commit_count);

    auto body_iid2 = InternedLogMessageBody::Get(&ctx, "Alas, poor Yorick!");
    EXPECT_EQ(body_iid, body_iid2);
    EXPECT_EQ(1, InternedLogMessageBody::commit_count);
  });
  TRACE_EVENT_END("foo");

  TRACE_EVENT_BEGIN("foo", "EventWithState", [&](perfetto::EventContext ctx) {
    // Check that very large amounts of interned data works.
    auto log = ctx.event()->set_log_message();
    log->set_body_iid(InternedLogMessageBody::Get(&ctx, large_message.str()));
    EXPECT_EQ(2, InternedLogMessageBody::commit_count);
  });
  TRACE_EVENT_END("foo");

  // Make sure interned data persists across trace points.
  TRACE_EVENT_BEGIN("foo", "EventWithState", [&](perfetto::EventContext ctx) {
    auto body_iid2 = InternedLogMessageBody::Get(&ctx, "Alas, poor Yorick!");
    EXPECT_EQ(body_iid, body_iid2);

    auto body_iid3 = InternedLogMessageBody::Get(&ctx, "I knew him, Horatio");
    EXPECT_NE(body_iid, body_iid3);
    auto log = ctx.event()->set_log_message();
    log->set_body_iid(body_iid3);
    EXPECT_EQ(3, InternedLogMessageBody::commit_count);
  });
  TRACE_EVENT_END("foo");

  tracing_session->get()->StopBlocking();
  auto log_messages = ReadLogMessagesFromTrace(tracing_session->get());
  EXPECT_THAT(log_messages,
              ElementsAre("Alas, poor Yorick!", large_message.str(),
                          "I knew him, Horatio"));
}

struct InternedLogMessageBodySmall
    : public perfetto::TrackEventInternedDataIndex<
          InternedLogMessageBodySmall,
          perfetto::protos::pbzero::InternedData::kLogMessageBodyFieldNumber,
          const char*,
          perfetto::SmallInternedDataTraits> {
  static void Add(perfetto::protos::pbzero::InternedData* interned_data,
                  size_t iid,
                  const char* value) {
    auto l = interned_data->add_log_message_body();
    l->set_iid(iid);
    l->set_body(value);
  }
};

TEST_F(PerfettoApiTest, TrackEventTypedArgsWithInterningByValue) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  size_t body_iid;
  TRACE_EVENT_BEGIN("foo", "EventWithState", [&](perfetto::EventContext ctx) {
    body_iid = InternedLogMessageBodySmall::Get(&ctx, "This above all:");
    auto log = ctx.event()->set_log_message();
    log->set_body_iid(body_iid);

    auto body_iid2 = InternedLogMessageBodySmall::Get(&ctx, "This above all:");
    EXPECT_EQ(body_iid, body_iid2);

    auto body_iid3 =
        InternedLogMessageBodySmall::Get(&ctx, "to thine own self be true");
    EXPECT_NE(body_iid, body_iid3);
  });
  TRACE_EVENT_END("foo");

  tracing_session->get()->StopBlocking();
  auto log_messages = ReadLogMessagesFromTrace(tracing_session->get());
  EXPECT_THAT(log_messages, ElementsAre("This above all:"));
}

struct InternedLogMessageBodyHashed
    : public perfetto::TrackEventInternedDataIndex<
          InternedLogMessageBodyHashed,
          perfetto::protos::pbzero::InternedData::kLogMessageBodyFieldNumber,
          std::string,
          perfetto::HashedInternedDataTraits> {
  static void Add(perfetto::protos::pbzero::InternedData* interned_data,
                  size_t iid,
                  const std::string& value) {
    auto l = interned_data->add_log_message_body();
    l->set_iid(iid);
    l->set_body(value.data(), value.size());
  }
};

TEST_F(PerfettoApiTest, TrackEventTypedArgsWithInterningByHashing) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  size_t body_iid;
  TRACE_EVENT_BEGIN(
      "foo", "EventWithState", [&](perfetto::EventContext ctx) {
        // Test using a dynamically created interned value.
        body_iid = InternedLogMessageBodyHashed::Get(
            &ctx, std::string("Though this ") + "be madness,");
        auto log = ctx.event()->set_log_message();
        log->set_body_iid(body_iid);

        auto body_iid2 =
            InternedLogMessageBodyHashed::Get(&ctx, "Though this be madness,");
        EXPECT_EQ(body_iid, body_iid2);

        auto body_iid3 =
            InternedLogMessageBodyHashed::Get(&ctx, "yet there is method in’t");
        EXPECT_NE(body_iid, body_iid3);
      });
  TRACE_EVENT_END("foo");

  tracing_session->get()->StopBlocking();
  auto log_messages = ReadLogMessagesFromTrace(tracing_session->get());
  EXPECT_THAT(log_messages, ElementsAre("Though this be madness,"));
}

struct InternedSourceLocation
    : public perfetto::TrackEventInternedDataIndex<
          InternedSourceLocation,
          perfetto::protos::pbzero::InternedData::kSourceLocationsFieldNumber,
          SourceLocation> {
  static void Add(perfetto::protos::pbzero::InternedData* interned_data,
                  size_t iid,
                  const SourceLocation& value) {
    auto l = interned_data->add_source_locations();
    auto file_name = std::get<0>(value);
    auto function_name = std::get<1>(value);
    auto line_number = std::get<2>(value);
    l->set_iid(iid);
    l->set_file_name(file_name);
    l->set_function_name(function_name);
    l->set_line_number(line_number);
  }
};

TEST_F(PerfettoApiTest, TrackEventTypedArgsWithInterningComplexValue) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  TRACE_EVENT_BEGIN("foo", "EventWithState", [&](perfetto::EventContext ctx) {
    const SourceLocation location{"file.cc", "SomeFunction", 123};
    auto location_iid = InternedSourceLocation::Get(&ctx, location);
    auto body_iid = InternedLogMessageBody::Get(&ctx, "To be, or not to be");
    auto log = ctx.event()->set_log_message();
    log->set_source_location_iid(location_iid);
    log->set_body_iid(body_iid);

    auto location_iid2 = InternedSourceLocation::Get(&ctx, location);
    EXPECT_EQ(location_iid, location_iid2);

    const SourceLocation location2{"file.cc", "SomeFunction", 456};
    auto location_iid3 = InternedSourceLocation::Get(&ctx, location2);
    EXPECT_NE(location_iid, location_iid3);
  });
  TRACE_EVENT_END("foo");

  tracing_session->get()->StopBlocking();
  auto log_messages = ReadLogMessagesFromTrace(tracing_session->get());
  EXPECT_THAT(log_messages,
              ElementsAre("SomeFunction(file.cc:123): To be, or not to be"));
}

TEST_F(PerfettoApiTest, TrackEventScoped) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  {
    uint64_t arg = 123;
    TRACE_EVENT("test", "TestEventWithArgs", [&](perfetto::EventContext ctx) {
      ctx.event()->set_log_message()->set_body_iid(arg);
    });
  }

  // Ensure a single line if statement counts as a valid scope for the macro.
  if (true)
    TRACE_EVENT("test", "SingleLineTestEvent");

  {
    // Make sure you can have multiple scoped events in the same scope.
    TRACE_EVENT("test", "TestEvent");
    TRACE_EVENT("test", "AnotherEvent");
    TRACE_EVENT("foo", "DisabledEvent");
  }
  perfetto::TrackEvent::Flush();

  tracing_session->get()->StopBlocking();
  auto slices = ReadSlicesFromTrace(tracing_session->get());
  EXPECT_THAT(slices, ElementsAre("B:test.TestEventWithArgs", "E:test.",
                                  "B:test.SingleLineTestEvent", "E:test.",
                                  "B:test.TestEvent", "B:test.AnotherEvent",
                                  "E:test.", "E:test."));
}

TEST_F(PerfettoApiTest, TrackEventInstant) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  TRACE_EVENT_INSTANT("test", "TestEvent");
  TRACE_EVENT_INSTANT("test", "AnotherEvent");
  perfetto::TrackEvent::Flush();

  tracing_session->get()->StopBlocking();
  auto slices = ReadSlicesFromTrace(tracing_session->get());
  EXPECT_THAT(slices, ElementsAre("I:test.TestEvent", "I:test.AnotherEvent"));
}

TEST_F(PerfettoApiTest, TrackEventDebugAnnotations) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  TRACE_EVENT_BEGIN("test", "E", "bool_arg", false);
  TRACE_EVENT_BEGIN("test", "E", "int_arg", -123);
  TRACE_EVENT_BEGIN("test", "E", "uint_arg", 456u);
  TRACE_EVENT_BEGIN("test", "E", "float_arg", 3.14159262f);
  TRACE_EVENT_BEGIN("test", "E", "double_arg", 6.22);
  TRACE_EVENT_BEGIN("test", "E", "str_arg", "hello", "str_arg2",
                    std::string("tracing"));
  TRACE_EVENT_BEGIN("test", "E", "ptr_arg",
                    reinterpret_cast<void*>(0xbaadf00d));
  perfetto::TrackEvent::Flush();

  tracing_session->get()->StopBlocking();
  auto slices = ReadSlicesFromTrace(tracing_session->get());
  EXPECT_THAT(
      slices,
      ElementsAre("B:test.E(bool_arg=(bool)0)", "B:test.E(int_arg=(int)-123)",
                  "B:test.E(uint_arg=(uint)456)",
                  "B:test.E(float_arg=(double)3.14159)",
                  "B:test.E(double_arg=(double)6.22)",
                  "B:test.E(str_arg=(string)hello,str_arg2=(string)tracing)",
                  "B:test.E(ptr_arg=(pointer)baadf00d)"));
}

TEST_F(PerfettoApiTest, TrackEventCustomDebugAnnotations) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");

  class MyDebugAnnotation : public perfetto::DebugAnnotation {
   public:
    ~MyDebugAnnotation() override = default;

    void Add(
        perfetto::protos::pbzero::DebugAnnotation* annotation) const override {
      annotation->set_legacy_json_value(R"({"key": 123})");
    }
  };

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  TRACE_EVENT_BEGIN("test", "E", "custom_arg", MyDebugAnnotation());
  TRACE_EVENT_BEGIN("test", "E", "normal_arg", "x", "custom_arg",
                    MyDebugAnnotation());
  perfetto::TrackEvent::Flush();

  tracing_session->get()->StopBlocking();
  auto slices = ReadSlicesFromTrace(tracing_session->get());
  EXPECT_THAT(
      slices,
      ElementsAre(
          R"(B:test.E(custom_arg=(json){"key": 123}))",
          R"(B:test.E(normal_arg=(string)x,custom_arg=(json){"key": 123}))"));
}

TEST_F(PerfettoApiTest, TrackEventCustomRawDebugAnnotations) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("test");

  // Note: this class is also testing a non-moveable and non-copiable argument.
  class MyRawDebugAnnotation : public perfetto::DebugAnnotation {
   public:
    MyRawDebugAnnotation() { msg_->set_string_value("nested_value"); }
    ~MyRawDebugAnnotation() = default;

    // |msg_| already deletes these implicitly, but let's be explicit for safety
    // against future changes.
    MyRawDebugAnnotation(const MyRawDebugAnnotation&) = delete;
    MyRawDebugAnnotation(MyRawDebugAnnotation&&) = delete;

    void Add(perfetto::protos::pbzero::DebugAnnotation* annotation) const {
      auto ranges = msg_.GetRanges();
      annotation->AppendScatteredBytes(
          perfetto::protos::pbzero::DebugAnnotation::kNestedValueFieldNumber,
          &ranges[0], ranges.size());
    }

   private:
    mutable protozero::HeapBuffered<
        perfetto::protos::pbzero::DebugAnnotation::NestedValue>
        msg_;
  };

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  TRACE_EVENT_BEGIN("test", "E", "raw_arg", MyRawDebugAnnotation());
  TRACE_EVENT_BEGIN("test", "E", "plain_arg", 42, "raw_arg",
                    MyRawDebugAnnotation());
  perfetto::TrackEvent::Flush();

  tracing_session->get()->StopBlocking();
  auto slices = ReadSlicesFromTrace(tracing_session->get());
  EXPECT_THAT(
      slices,
      ElementsAre("B:test.E(raw_arg=(nested)nested_value)",
                  "B:test.E(plain_arg=(int)42,raw_arg=(nested)nested_value)"));
}

TEST_F(PerfettoApiTest, TrackEventArgumentsNotEvaluatedWhenDisabled) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  ds_cfg->set_legacy_config("foo");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  bool called = false;
  auto ArgumentFunction = [&] {
    called = true;
    return 123;
  };

  TRACE_EVENT_BEGIN("test", "DisabledEvent", "arg", ArgumentFunction());
  { TRACE_EVENT("test", "DisabledScopedEvent", "arg", ArgumentFunction()); }
  perfetto::TrackEvent::Flush();

  tracing_session->get()->StopBlocking();
  EXPECT_FALSE(called);

  ArgumentFunction();
  EXPECT_TRUE(called);
}

TEST_F(PerfettoApiTest, OneDataSourceOneEvent) {
  auto* data_source = &data_sources_["my_data_source"];

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");
  ds_cfg->set_legacy_config("test config");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);

  MockDataSource::Trace([](MockDataSource::TraceContext) {
    FAIL() << "Should not be called because the trace was not started";
  });
  MockDataSource::CallIfEnabled([](uint32_t) {
    FAIL() << "Should not be called because the trace was not started";
  });

  tracing_session->get()->Start();
  data_source->on_setup.Wait();
  EXPECT_EQ(data_source->config.legacy_config(), "test config");
  data_source->on_start.Wait();

  // Emit one trace event.
  std::atomic<int> trace_lambda_calls{0};
  MockDataSource::Trace(
      [&trace_lambda_calls](MockDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(42);
        packet->set_for_testing()->set_str("event 1");
        trace_lambda_calls++;
        packet->Finalize();

        // The SMB scraping logic will skip the last packet because it cannot
        // guarantee it's finalized. Create an empty packet so we get the
        // previous one and this empty one is ignored.
        packet = ctx.NewTracePacket();
      });

  uint32_t active_instances = 0;
  MockDataSource::CallIfEnabled([&active_instances](uint32_t instances) {
    active_instances = instances;
  });
  EXPECT_EQ(1u, active_instances);

  data_source->on_stop.Wait();
  tracing_session->on_stop.Wait();
  EXPECT_EQ(trace_lambda_calls, 1);

  MockDataSource::Trace([](MockDataSource::TraceContext) {
    FAIL() << "Should not be called because the trace is now stopped";
  });
  MockDataSource::CallIfEnabled([](uint32_t) {
    FAIL() << "Should not be called because the trace is now stopped";
  });

  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  ASSERT_GE(raw_trace.size(), 0u);

  perfetto::protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));
  bool test_packet_found = false;
  for (const auto& packet : trace.packet()) {
    if (!packet.has_for_testing())
      continue;
    EXPECT_FALSE(test_packet_found);
    EXPECT_EQ(packet.timestamp(), 42U);
    EXPECT_EQ(packet.for_testing().str(), "event 1");
    test_packet_found = true;
  }
  EXPECT_TRUE(test_packet_found);
}

TEST_F(PerfettoApiTest, BlockingStartAndStop) {
  auto* data_source = &data_sources_["my_data_source"];

  // Register a second data source to get a bit more coverage.
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("my_data_source2");
  MockDataSource2::Register(dsd);

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");
  ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source2");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);

  tracing_session->get()->StartBlocking();
  EXPECT_TRUE(data_source->on_setup.notified());
  EXPECT_TRUE(data_source->on_start.notified());

  tracing_session->get()->StopBlocking();
  EXPECT_TRUE(data_source->on_stop.notified());
  EXPECT_TRUE(tracing_session->on_stop.notified());
}

TEST_F(PerfettoApiTest, BlockingStartAndStopOnEmptySession) {
  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("non_existent_data_source");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();
  tracing_session->get()->StopBlocking();
  EXPECT_TRUE(tracing_session->on_stop.notified());
}

TEST_F(PerfettoApiTest, WriteEventsAfterDeferredStop) {
  auto* data_source = &data_sources_["my_data_source"];
  data_source->handle_stop_asynchronously = true;

  // Setup the trace config and start the tracing session.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // Stop and wait for the producer to have seen the stop event.
  WaitableTestEvent consumer_stop_signal;
  tracing_session->get()->SetOnStopCallback(
      [&consumer_stop_signal] { consumer_stop_signal.Notify(); });
  tracing_session->get()->Stop();
  data_source->on_stop.Wait();

  // At this point tracing should be still allowed because of the
  // HandleStopAsynchronously() call.
  bool lambda_called = false;

  // This usleep is here just to prevent that we accidentally pass the test
  // just by virtue of hitting some race. We should be able to trace up until
  // 5 seconds after seeing the stop when using the deferred stop mechanism.
  usleep(250 * 1000);

  MockDataSource::Trace([&lambda_called](MockDataSource::TraceContext ctx) {
    auto packet = ctx.NewTracePacket();
    packet->set_for_testing()->set_str("event written after OnStop");
    packet->Finalize();
    ctx.Flush();
    lambda_called = true;
  });
  ASSERT_TRUE(lambda_called);

  // Now call the async stop closure. This acks the stop to the service and
  // disallows further Trace() calls.
  EXPECT_TRUE(data_source->async_stop_closure);
  data_source->async_stop_closure();

  // Wait that the stop is propagated to the consumer.
  consumer_stop_signal.Wait();

  MockDataSource::Trace([](MockDataSource::TraceContext) {
    FAIL() << "Should not be called after the stop is acked";
  });

  // Check the contents of the trace.
  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  ASSERT_GE(raw_trace.size(), 0u);
  perfetto::protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));
  int test_packet_found = 0;
  for (const auto& packet : trace.packet()) {
    if (!packet.has_for_testing())
      continue;
    EXPECT_EQ(packet.for_testing().str(), "event written after OnStop");
    test_packet_found++;
  }
  EXPECT_EQ(test_packet_found, 1);
}

TEST_F(PerfettoApiTest, RepeatedStartAndStop) {
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");

  for (int i = 0; i < 5; i++) {
    auto* tracing_session = NewTrace(cfg);
    tracing_session->get()->Start();
    std::atomic<bool> stop_called{false};
    tracing_session->get()->SetOnStopCallback(
        [&stop_called] { stop_called = true; });
    tracing_session->get()->StopBlocking();
    EXPECT_TRUE(stop_called);
  }
}

TEST_F(PerfettoApiTest, SetupWithFile) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  char temp_file[] = "/data/local/tmp/perfetto-XXXXXXXX";
#else
  char temp_file[] = "/tmp/perfetto-XXXXXXXX";
#endif
  int fd = mkstemp(temp_file);
  ASSERT_TRUE(fd >= 0);

  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");
  // Write a trace into |fd|.
  auto* tracing_session = NewTrace(cfg, fd);
  tracing_session->get()->StartBlocking();
  tracing_session->get()->StopBlocking();
  // Check that |fd| didn't get closed.
  EXPECT_EQ(0, fcntl(fd, F_GETFD, 0));
  // Check that the trace got written.
  EXPECT_GT(lseek(fd, 0, SEEK_END), 0);
  EXPECT_EQ(0, close(fd));
  // Clean up.
  EXPECT_EQ(0, unlink(temp_file));
}

TEST_F(PerfettoApiTest, MultipleRegistrations) {
  // Attempt to register the same data source again.
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("my_data_source");
  EXPECT_TRUE(MockDataSource::Register(dsd));

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // Emit one trace event.
  std::atomic<int> trace_lambda_calls{0};
  MockDataSource::Trace([&trace_lambda_calls](MockDataSource::TraceContext) {
    trace_lambda_calls++;
  });

  // Make sure the data source got called only once.
  tracing_session->get()->StopBlocking();
  EXPECT_EQ(trace_lambda_calls, 1);
}

TEST_F(PerfettoApiTest, CustomIncrementalState) {
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("incr_data_source");
  TestIncrementalDataSource::Register(dsd);

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(500);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("incr_data_source");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();

  // First emit a no-op trace event that initializes the incremental state as a
  // side effect.
  TestIncrementalDataSource::Trace(
      [](TestIncrementalDataSource::TraceContext) {});
  EXPECT_TRUE(TestIncrementalState::constructed);

  // Check that the incremental state is carried across trace events.
  TestIncrementalDataSource::Trace(
      [](TestIncrementalDataSource::TraceContext ctx) {
        auto* state = ctx.GetIncrementalState();
        EXPECT_TRUE(state);
        EXPECT_EQ(100, state->count);
        state->count++;
      });

  TestIncrementalDataSource::Trace(
      [](TestIncrementalDataSource::TraceContext ctx) {
        auto* state = ctx.GetIncrementalState();
        EXPECT_EQ(101, state->count);
      });

  // Make sure the incremental state gets cleaned up between sessions.
  tracing_session->get()->StopBlocking();
  tracing_session = NewTrace(cfg);
  tracing_session->get()->StartBlocking();
  TestIncrementalDataSource::Trace(
      [](TestIncrementalDataSource::TraceContext ctx) {
        auto* state = ctx.GetIncrementalState();
        EXPECT_TRUE(TestIncrementalState::destroyed);
        EXPECT_TRUE(state);
        EXPECT_EQ(100, state->count);
      });
  tracing_session->get()->StopBlocking();
}

// Regression test for b/139110180. Checks that GetDataSourceLocked() can be
// called from OnStart() and OnStop() callbacks without deadlocking.
TEST_F(PerfettoApiTest, GetDataSourceLockedFromCallbacks) {
  auto* data_source = &data_sources_["my_data_source"];

  // Setup the trace config.
  perfetto::TraceConfig cfg;
  cfg.set_duration_ms(1);
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("my_data_source");

  // Create a new trace session.
  auto* tracing_session = NewTrace(cfg);

  data_source->on_start_callback = [] {
    MockDataSource::Trace([](MockDataSource::TraceContext ctx) {
      ctx.NewTracePacket()->set_for_testing()->set_str("on-start");
      auto ds = ctx.GetDataSourceLocked();
      ASSERT_TRUE(!!ds);
      ctx.NewTracePacket()->set_for_testing()->set_str("on-start-locked");
    });
  };

  data_source->on_stop_callback = [] {
    MockDataSource::Trace([](MockDataSource::TraceContext ctx) {
      ctx.NewTracePacket()->set_for_testing()->set_str("on-stop");
      auto ds = ctx.GetDataSourceLocked();
      ASSERT_TRUE(!!ds);
      ctx.NewTracePacket()->set_for_testing()->set_str("on-stop-locked");
      ctx.Flush();
    });
  };

  tracing_session->get()->Start();
  data_source->on_stop.Wait();
  tracing_session->on_stop.Wait();

  std::vector<char> raw_trace = tracing_session->get()->ReadTraceBlocking();
  ASSERT_GE(raw_trace.size(), 0u);

  perfetto::protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromArray(raw_trace.data(), int(raw_trace.size())));
  int packets_found = 0;
  for (const auto& packet : trace.packet()) {
    if (!packet.has_for_testing())
      continue;
    packets_found |= packet.for_testing().str() == "on-start" ? 1 : 0;
    packets_found |= packet.for_testing().str() == "on-start-locked" ? 2 : 0;
    packets_found |= packet.for_testing().str() == "on-stop" ? 4 : 0;
    packets_found |= packet.for_testing().str() == "on-stop-locked" ? 8 : 0;
  }
  EXPECT_EQ(packets_found, 1 | 2 | 4 | 8);
}

}  // namespace

PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(MockDataSource);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(MockDataSource2);
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(TestIncrementalDataSource,
                                           TestIncrementalDataSourceTraits);
