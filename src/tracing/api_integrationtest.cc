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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing.h"

// Deliberately not pulling any non-public perfetto header to spot accidental
// header public -> non-public dependency while building this file.

// TODO(primiano): move these generated classes to /public/.
#include "perfetto/ext/tracing/core/data_source_descriptor.h"
#include "perfetto/ext/tracing/core/trace_config.h"

namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::NiceMock;
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
};

class MockDataSource : public perfetto::DataSource<MockDataSource> {
 public:
  void OnSetup(const SetupArgs&) override;
  void OnStart(const StartArgs&) override;
  void OnStop(const StopArgs&) override;
  TestDataSourceHandle* handle_ = nullptr;
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
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
  }

  void TearDown() override { instance = nullptr; }

  template <typename DataSourceType>
  TestDataSourceHandle* RegisterDataSource(std::string name) {
    EXPECT_EQ(data_sources_.count(name), 0);
    TestDataSourceHandle* handle = &data_sources_[name];
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name(name);
    DataSourceType::Register(dsd);
    return handle;
  }

  TestTracingSessionHandle* NewTrace(const perfetto::TraceConfig& cfg) {
    sessions_.emplace_back();
    TestTracingSessionHandle* handle = &sessions_.back();
    handle->session =
        perfetto::Tracing::NewTrace(perfetto::BackendType::kInProcessBackend);
    handle->session->SetOnStopCallback([handle] { handle->on_stop.Notify(); });
    handle->session->Setup(cfg);
    return handle;
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
  handle_->on_start.Notify();
}

void MockDataSource::OnStop(const StopArgs&) {
  EXPECT_NE(handle_, nullptr);
  handle_->on_stop.Notify();
}

// -------------
// Test fixtures
// -------------
TEST_F(PerfettoApiTest, OneDataSourceOneEvent) {
  auto* data_source = RegisterDataSource<MockDataSource>("my_data_source");

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

  data_source->on_stop.Wait();
  tracing_session->on_stop.Wait();
  EXPECT_EQ(trace_lambda_calls, 1);

  MockDataSource::Trace([](MockDataSource::TraceContext) {
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

}  // namespace

PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(MockDataSource);
