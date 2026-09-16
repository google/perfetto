/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/tracing/tracing.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/tracing/backend_type.h"
#include "perfetto/tracing/console_interceptor.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/in_process_tracing_backend.h"
#include "perfetto/tracing/platform.h"
#include "perfetto/tracing/tracing_backend.h"
#include "protos/perfetto/config/interceptor_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trigger.gen.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/internal/tracing_muxer_impl.h"
#include "src/tracing/test/api_test_support.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "src/tracing/test/proxy_producer_endpoint.h"
#include "src/tracing/v2/producer_ring.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::test {

class TracingMuxerImplV2Test : public testing::Test {
 protected:
  using ProducerImpl = internal::TracingMuxerImpl::ProducerImpl;

  static constexpr uint32_t kTestNumChunks = 32;
  static constexpr uint32_t kTestChunkSize = 256;

  template <typename Callback>
  static void RunOnMuxerAndWait(Callback callback) {
    base::WaitableEvent done;
    TracingMuxerImplInternalsForTest::PostToMuxerSequence(
        [callback = std::move(callback), &done] {
          auto* muxer = static_cast<internal::TracingMuxerImpl*>(
              internal::TracingMuxerImpl::Get());
          callback(muxer);
          done.Notify();
        });
    done.Wait();
  }

  // The v2 connection of any connected producer, or null.
  static std::shared_ptr<ProducerImpl::TracingV2Connection> GetConnection() {
    std::shared_ptr<ProducerImpl::TracingV2Connection> connection;
    RunOnMuxerAndWait([&](auto* muxer) {
      for (auto& backend : muxer->producer_backends_) {
        if (backend.producer->tracing_v2_connection_)
          connection = backend.producer->tracing_v2_connection_;
      }
    });
    return connection;
  }

  // Holds an accepted drain reply after the muxer has requested the barrier.
  static std::function<void(bool)> HoldDrain(std::function<void()> completion) {
    std::function<void(bool)> reply;
    RunOnMuxerAndWait([&](auto* muxer) {
      auto endpoint =
          std::make_shared<testing::StrictMock<MockProducerEndpoint>>();
      auto connection = std::make_shared<ProducerImpl::TracingV2Connection>();
      connection->endpoint = endpoint;
      EXPECT_CALL(*endpoint, NotifyTracingV2RingData(testing::_))
          .WillOnce(
              [&](std::function<void(bool)> cb) { reply = std::move(cb); });
      muxer->DrainTracingV2RingBufferThenPostToMuxer(connection,
                                                     std::move(completion));
    });
    return reply;
  }

  // Number of flush requests the connected producers are still tracking.
  static size_t PendingFlushCount() {
    size_t count = 0;
    RunOnMuxerAndWait([&](auto* muxer) {
      for (auto& backend : muxer->producer_backends_)
        count += backend.producer->pending_flushes_.size();
    });
    return count;
  }

  // Calls ProducerImpl::Flush() for |instance_id| on the muxer thread.
  // Returns after that call, which can leave callbacks or a ring drain pending.
  static void FlushInstanceOnProducer(internal::TracingBackendId backend_id,
                                      DataSourceInstanceID instance_id,
                                      FlushRequestID flush_id) {
    RunOnMuxerAndWait([&](auto* muxer) {
      muxer->FindProducerBackendById(backend_id)
          ->producer->Flush(flush_id, &instance_id, 1, FlushFlags());
    });
  }

  // Installs a v2 connection with a real ring and no service reader.
  // This routes flushes through the ordered v2 queue. These tests neither write
  // to the ring nor drain it.
  static void InstallTracingV2Connection(ProducerImpl* producer) {
    auto connection = std::make_shared<ProducerImpl::TracingV2Connection>();
    connection->endpoint = producer->service_;
    auto memory = std::shared_ptr<SharedMemory>(
        InProcessSharedMemory::Create(static_cast<size_t>(
            tracing_v2::RingLogicalSize(kTestNumChunks, kTestChunkSize))));
    connection->ring = tracing_v2::ProducerRing::Create(
        memory, kTestNumChunks, kTestChunkSize, connection.get());
    std::atomic_store(&producer->tracing_v2_connection_, std::move(connection));
  }

  // Drives EnsureTracingV2Connection() with a mock endpoint and checks the ring
  // it builds for a 256 KiB negotiated budget and the requested chunk size.
  static void CheckConnectionSizing(uint32_t requested_chunk_size,
                                    uint32_t expected_chunk_size,
                                    uint32_t expected_num_chunks) {
    RunOnMuxerAndWait([&](auto* muxer) {
      ProducerImpl producer(muxer, 0, 0, false);
      auto endpoint =
          std::make_shared<testing::StrictMock<MockProducerEndpoint>>();
      producer.service_ = endpoint;
      EXPECT_CALL(*endpoint, IsTracingV2DirectTransportSupported())
          .WillOnce(testing::Return(true));
      EXPECT_CALL(*endpoint, tracing_v2_chunk_size_bytes())
          .WillOnce(testing::Return(requested_chunk_size));
      EXPECT_CALL(*endpoint, tracing_v2_ring_size_bytes())
          .WillOnce(testing::Return(256 * 1024));
      EXPECT_CALL(*endpoint, CreateTracingV2Ring(testing::_))
          .WillOnce([](size_t size) {
            return std::shared_ptr<SharedMemory>(
                InProcessSharedMemory::Create(size));
          });
      EXPECT_CALL(*endpoint, AdoptTracingV2Ring(testing::_, testing::_));
      producer.EnsureTracingV2Connection();
      tracing_v2::ProducerRing* ring =
          producer.tracing_v2_connection_->ring.get();
      ASSERT_NE(ring, nullptr);
      EXPECT_EQ(ring->chunk_size(), expected_chunk_size);
      EXPECT_EQ(ring->num_chunks(), expected_num_chunks);
      // A second call is a no-op and does not touch the endpoint (StrictMock).
      producer.EnsureTracingV2Connection();
    });
  }

  static void SetupStartupInstanceOnV2Connection() {
    RunOnMuxerAndWait([](auto* muxer) {
      for (auto& backend : muxer->producer_backends_) {
        if (!backend.producer->tracing_v2_connection_)
          continue;
        for (const auto& rds : muxer->data_sources_) {
          if (rds.descriptor.name() != "tracing_v2_flushing")
            continue;
          DataSourceConfig config;
          config.set_name(rds.descriptor.name());
          muxer->SetupDataSourceImpl(
              rds, backend.id, backend.producer->connection_id_.load(),
              /*instance_id=*/0, config, /*startup_session_id=*/1);
        }
      }
    });
  }
};

TEST_F(TracingMuxerImplV2Test, StartupReservationPreventsV2Connection) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        ProducerImpl producer(nullptr, 0, 0, false);
        // SetupDataSourceImpl increments this before handing out a startup
        // writer. It remains nonzero after adoption on the same connection.
        producer.last_startup_target_buffer_reservation_ = 1;
        producer.EnsureTracingV2Connection();
      },
      "cannot share a producer connection with startup tracing");
}

// Preserve v1 flush behavior before the connection selects v2:
// - Asynchronous completions acknowledge the completed prefix of the queue.
// - Synchronous requests are acked immediately, including all older requests.
// Remove those older requests locally so late callbacks are ignored.
TEST_F(TracingMuxerImplV2Test, NeverV2ConnectionKeepsV1FlushAcks) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  // There is no muxer, hence no OnFlush() to run. Set up the bookkeeping of
  // asynchronous flushes the way the v1 Flush() path leaves it.
  producer->pending_flushes_[1].pending_data_sources.insert(11);
  producer->pending_flushes_[2].pending_data_sources.insert(12);

  testing::InSequence in_sequence;
  EXPECT_CALL(*endpoint, NotifyFlushComplete(2));
  // Request 2 completes first but has to wait for 1.
  producer->NotifyFlushForDataSourceDone(12, 2);
  producer->NotifyFlushForDataSourceDone(11, 1);
  EXPECT_TRUE(producer->pending_flushes_.empty());

  producer->pending_flushes_[3].pending_data_sources.insert(13);
  EXPECT_CALL(*endpoint, NotifyFlushComplete(4));
  producer->Flush(4, nullptr, 0, FlushFlags());
  EXPECT_TRUE(producer->pending_flushes_.empty());
  // Nothing left to ack for request 3.
  producer->NotifyFlushForDataSourceDone(13, 3);
  EXPECT_FALSE(producer->tracing_v2_connection_);
}

// A newer synchronous v1 flush also acks older requests. Their late callbacks
// must be ignored, and they must not block the queue when v2 is selected.
TEST_F(TracingMuxerImplV2Test, CumulativelyAckedV1FlushDoesNotBlockV2Queue) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  producer->pending_flushes_[1].pending_data_sources.insert(11);

  testing::InSequence in_sequence;
  EXPECT_CALL(*endpoint, NotifyFlushComplete(2));
  EXPECT_CALL(*endpoint, NotifyFlushComplete(3));
  producer->Flush(2, nullptr, 0, FlushFlags());

  InstallTracingV2Connection(producer.get());
  producer->Flush(3, nullptr, 0, FlushFlags());
  EXPECT_TRUE(producer->pending_flushes_.empty());
  producer->NotifyFlushForDataSourceDone(11, 1);
  producer.reset();
}

// Requests of a disposed connection can't be acked anymore and must not be
// carried over to the next connection, with or without a v2 ring.
TEST_F(TracingMuxerImplV2Test, DisposeConnectionDropsPendingFlushes) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  producer->pending_flushes_[1].pending_data_sources.insert(11);
  producer->DisposeConnection();
  EXPECT_TRUE(producer->pending_flushes_.empty());
  EXPECT_FALSE(producer->service_);

  producer->service_ = endpoint;
  InstallTracingV2Connection(producer.get());
  // Nothing else holds the ring, so disposing the connection must free it.
  std::weak_ptr<tracing_v2::ProducerRing> weak_ring =
      producer->tracing_v2_connection_->ring;
  producer->pending_flushes_[1].pending_data_sources.insert(11);
  producer->pending_flushes_[2].ring_buffer_drain =
      ProducerImpl::RingBufferDrainState::kPending;
  producer->DisposeConnection();
  EXPECT_TRUE(producer->pending_flushes_.empty());
  EXPECT_FALSE(producer->tracing_v2_connection_);
  EXPECT_TRUE(weak_ring.expired());
}

// A v1 asynchronous flush still pending when the connection selects v2 is
// consumed by the ordered path: a later request queues behind it instead of
// overtaking it, and its completion acks both.
TEST_F(TracingMuxerImplV2Test, PendingV1FlushIsConsumedByTheV2Queue) {
  auto endpoint = std::make_shared<testing::NiceMock<MockProducerEndpoint>>();
  auto producer = std::make_unique<ProducerImpl>(nullptr, 0, 0, false);
  producer->service_ = endpoint;
  producer->connected_ = true;
  producer->pending_flushes_[1].pending_data_sources.insert(11);

  InstallTracingV2Connection(producer.get());
  EXPECT_CALL(*endpoint, NotifyFlushComplete(testing::_)).Times(0);
  producer->Flush(2, nullptr, 0, FlushFlags());
  EXPECT_EQ(producer->pending_flushes_.size(), 2u);
  testing::Mock::VerifyAndClearExpectations(endpoint.get());

  EXPECT_CALL(*endpoint, NotifyFlushComplete(2));
  producer->NotifyFlushForDataSourceDone(11, 1);
  EXPECT_TRUE(producer->pending_flushes_.empty());
  producer.reset();
}

}  // namespace perfetto::test

namespace perfetto {
namespace internal {
namespace {

using Internals = test::TracingMuxerImplInternalsForTest;

// Tests complete sessions through the in-process backend. Both writer modes
// produce length-delimited protobuf in the trace. Inspect the muxer's instance
// state to verify which writer the config selected.

class TracingV2TestDataSource
    : public perfetto::DataSource<TracingV2TestDataSource> {
 public:
  constexpr static bool kBufferExhaustedPolicyConfigurable = true;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
};

// Writes 2 MiB from OnFlush(), i.e. on the muxer thread, which the service also
// uses to drain the ring inline under backpressure.
class TracingV2PressureDataSource
    : public perfetto::DataSource<TracingV2PressureDataSource> {
 public:
  constexpr static bool kBufferExhaustedPolicyConfigurable = true;
  static std::atomic<uint64_t> drops;
  static std::atomic<base::WaitableEvent*> completed;
  void OnFlush(const FlushArgs&) override {
    const std::string payload(4096, 'p');
    for (uint32_t i = 0; i < 512; ++i) {
      Trace([&](TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str(payload);
      });
    }
    Trace([](TraceContext ctx) { drops.store(ctx.drop_count()); });
    if (auto* done = completed.exchange(nullptr))
      done->Notify();
  }
};
std::atomic<uint64_t> TracingV2PressureDataSource::drops{0};
std::atomic<base::WaitableEvent*> TracingV2PressureDataSource::completed{
    nullptr};

// No OnFlush() override, so Register() marks it no_flush.
class TracingV2NoFlushDataSource
    : public perfetto::DataSource<TracingV2NoFlushDataSource> {
 public:
  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// Counts OnFlush() calls and writes a packet from inside the callback.
class TracingV2FlushingDataSource
    : public perfetto::DataSource<TracingV2FlushingDataSource> {
 public:
  static std::atomic<uint32_t> flushes;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
  void OnFlush(const FlushArgs&) override {
    ++flushes;
    Trace([](TraceContext ctx) {
      ctx.NewTracePacket()->set_for_testing()->set_str("on flush");
    });
    if (base::WaitableEvent* done = on_flush_done.load())
      done->Notify();
  }

  // Set by a test that needs to know the producer has run this callback.
  static std::atomic<base::WaitableEvent*> on_flush_done;
};
std::atomic<uint32_t> TracingV2FlushingDataSource::flushes{0};
std::atomic<base::WaitableEvent*> TracingV2FlushingDataSource::on_flush_done{
    nullptr};

// Registers with no_flush despite an OnFlush() override. V2 drain requests
// must skip this callback. A simulated older service also tests the v1 path,
// which preserves the callback if the service explicitly requests a flush.
class TracingV2DeclaredNoFlushDataSource
    : public perfetto::DataSource<TracingV2DeclaredNoFlushDataSource> {
 public:
  static std::atomic<uint32_t> flushes;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
  void OnFlush(const FlushArgs&) override { ++flushes; }
};
std::atomic<uint32_t> TracingV2DeclaredNoFlushDataSource::flushes{0};

// Completes its stop asynchronously, when the test says so.
class TracingV2AsyncStopDataSource
    : public perfetto::DataSource<TracingV2AsyncStopDataSource> {
 public:
  static std::atomic<base::WaitableEvent*> stop_started;
  static std::mutex mutex;

  // Function-local so this file needs no exit-time destructor.
  static std::function<void()>& pending_stop() {
    static base::NoDestructor<std::function<void()>> instance;
    return instance.ref();
  }

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs& args) override { HoldStop(args); }

  static void HoldStop(const StopArgs& args) {
    base::WaitableEvent* started = stop_started.load();
    if (!started)
      return;
    std::function<void()> done = args.HandleStopAsynchronously();
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending_stop() = std::move(done);
    }
    started->Notify();
  }

  static void CompleteHeldStop() {
    stop_started.store(nullptr);
    std::function<void()> done;
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = std::move(pending_stop());
      pending_stop() = nullptr;
    }
    ASSERT_TRUE(done);
    done();
  }
};
std::atomic<base::WaitableEvent*> TracingV2AsyncStopDataSource::stop_started{
    nullptr};
std::mutex TracingV2AsyncStopDataSource::mutex;

// Holds one OnFlush() until the test releases it, and writes a packet right
// before completing it. Only the flush the test armed is held.
class TracingV2AsyncFlushDataSource
    : public perfetto::DataSource<TracingV2AsyncFlushDataSource> {
 public:
  // Armed by the test. While null, OnFlush() completes synchronously.
  static std::atomic<base::WaitableEvent*> hold_next_flush;
  static std::mutex mutex;

  // Function-local so this file needs no exit-time destructor.
  static std::function<void()>& pending_flush() {
    static base::NoDestructor<std::function<void()>> instance;
    return instance.ref();
  }

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs& args) override {
    TracingV2AsyncStopDataSource::HoldStop(args);
  }
  void OnFlush(const FlushArgs& args) override {
    base::WaitableEvent* started = hold_next_flush.load();
    if (!started)
      return;
    std::function<void()> done = args.HandleFlushAsynchronously();
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending_flush() = std::move(done);
    }
    started->Notify();
  }

  static void CompleteHeldFlush() {
    hold_next_flush.store(nullptr);
    // An asynchronous callback is allowed to write right up to the moment it
    // reports completion, so this packet has to be in the flushed trace.
    Trace([](TraceContext ctx) {
      ctx.NewTracePacket()->set_for_testing()->set_str("async flush");
    });
    std::function<void()> done;
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = std::move(pending_flush());
      pending_flush() = nullptr;
    }
    ASSERT_TRUE(done);
    done();
  }
};
std::atomic<base::WaitableEvent*>
    TracingV2AsyncFlushDataSource::hold_next_flush{nullptr};
std::mutex TracingV2AsyncFlushDataSource::mutex;

// Number of instances of |DerivedDataSource| that the muxer put on v2.
template <typename DerivedDataSource>
size_t CountInstancesUsingTracingV2() {
  DataSourceStaticState* static_state =
      perfetto::DataSourceHelper<DerivedDataSource>::type().static_state();
  size_t count = 0;
  for (size_t i = 0; i < kMaxDataSourceInstances; ++i) {
    DataSourceState* state = static_state->TryGet(i);
    if (state && state->use_tracing_v2)
      ++count;
  }
  return count;
}

// Re-executes this test so |body| starts with fresh producer connections.
// This also isolates Tracing::Shutdown(), which is terminal for the process.
// Assertion failures fail the child and are printed to stderr for the parent.
void RunInFreshProcess(std::function<void()> body) {
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_EXIT(
      {
        body();
        const testing::TestResult* result =
            testing::UnitTest::GetInstance()->current_test_info()->result();
        for (int i = 0; i < result->total_part_count(); ++i) {
          const testing::TestPartResult& part = result->GetTestPartResult(i);
          if (part.failed()) {
            PERFETTO_ELOG("%s:%d: %s", part.file_name(), part.line_number(),
                          part.message());
          }
        }
        std::_Exit(result->Failed() ? 1 : 0);
      },
      testing::ExitedWithCode(0), "");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

// Initialize()/ResetForTesting() per test does not work with the fake backend
// in this binary, so the suite initializes once.
class TracingV2InProcessTest : public test::TracingMuxerImplV2Test {
 protected:
  static void SetUpTestSuite() {
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name("tracing_v2_test");
    TracingV2TestDataSource::Register(dsd);
    dsd.set_name("tracing_v2_pressure");
    TracingV2PressureDataSource::Register(dsd);
    dsd.set_name("tracing_v2_no_flush");
    TracingV2NoFlushDataSource::Register(dsd);
    dsd.set_name("tracing_v2_flushing");
    TracingV2FlushingDataSource::Register(dsd);
    dsd.set_name("tracing_v2_async_flush");
    TracingV2AsyncFlushDataSource::Register(dsd);
    dsd.set_name("tracing_v2_async_stop");
    TracingV2AsyncStopDataSource::Register(dsd);
    dsd.set_name("tracing_v2_declared_no_flush");
    dsd.set_no_flush(true);
    TracingV2DeclaredNoFlushDataSource::Register(dsd);
    // TracingMuxerImpl only accepts a fixed set of interceptor names, and
    // "test_interceptor" already belongs to another suite in this binary.
    perfetto::ConsoleInterceptor::Register();
    perfetto::test::SyncProducers();
    perfetto::test::DisableReconnectLimit();
  }

  static void TearDownTestSuite() {
    // The data source's thread-local state points at muxer state that
    // ResetForTesting() is about to destroy.
    Internals::ClearDataSourceTlsStateOnReset<TracingV2TestDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2NoFlushDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2FlushingDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2AsyncFlushDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2AsyncStopDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<
        TracingV2DeclaredNoFlushDataSource>();
    Internals::ClearDataSourceTlsStateOnReset<TracingV2PressureDataSource>();
    perfetto::Tracing::ResetForTesting();
  }

  void TearDown() override {
    TracingV2AsyncFlushDataSource::hold_next_flush.store(nullptr);
    TracingV2FlushingDataSource::on_flush_done.store(nullptr);
    TracingV2AsyncStopDataSource::stop_started.store(nullptr);
  }

  // Returns once everything already queued on the muxer sequence has run.
  static void WaitForMuxerSequence() {
    base::WaitableEvent done;
    Internals::PostToMuxerSequence([&done] { done.Notify(); });
    done.Wait();
  }

  // Whether any connected producer owns a ring for direct v2 transport.
  static bool HasV2Connection() {
    WaitForMuxerSequence();
    return GetConnection() != nullptr;
  }

  enum class WriterSelection { kAbsent, kV1, kV2 };

  // Put the shared connection on the v2 control path explicitly so callers
  // are independent of test order. It stays on that path until disconnect.
  static void PutConnectionOnTracingV2() {
    StopAndParse(
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2))
            .get());
  }

  // Flushes the only active instance of |DerivedDataSource| the way an older
  // service would, i.e. without looking at no_flush.
  template <typename DerivedDataSource>
  static void FlushInstanceLikeAnOlderService(FlushRequestID flush_id) {
    DataSourceStaticState* static_state =
        perfetto::DataSourceHelper<DerivedDataSource>::type().static_state();
    DataSourceState* state = nullptr;
    for (size_t i = 0; i < kMaxDataSourceInstances; ++i) {
      if (DataSourceState* candidate = static_state->TryGet(i)) {
        ASSERT_EQ(state, nullptr);
        state = candidate;
      }
    }
    ASSERT_NE(state, nullptr);
    FlushInstanceOnProducer(state->backend_id, state->data_source_instance_id,
                            flush_id);
  }

  static perfetto::TraceConfig MakeConfigFor(
      std::initializer_list<const char*> data_source_names,
      WriterSelection selection) {
    perfetto::TraceConfig cfg;
    auto* buffer = cfg.add_buffers();
    buffer->set_size_kb(1024);
    // Direct v2 transport admits raw fragments straight into TraceBufferV2, so
    // a v2 data source needs a v2 destination buffer.
    if (selection == WriterSelection::kV2) {
      buffer->set_experimental_mode(
          perfetto::protos::gen::TraceConfig::BufferConfig::TRACE_BUFFER_V2);
    }
    for (const char* name : data_source_names) {
      auto* ds_cfg = cfg.add_data_sources()->mutable_config();
      ds_cfg->set_name(name);
      if (selection != WriterSelection::kAbsent)
        ds_cfg->set_use_tracing_v2(selection == WriterSelection::kV2);
    }
    return cfg;
  }

  static perfetto::TraceConfig MakeConfig() {
    return MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2);
  }

  static std::unique_ptr<perfetto::TracingSession> StartSession(
      const perfetto::TraceConfig& cfg) {
    auto session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
    session->Setup(cfg);
    session->StartBlocking();
    return session;
  }

  static std::vector<protos::gen::TracePacket> ReadTestPackets(
      perfetto::TracingSession* session) {
    const std::vector<char> bytes = session->ReadTraceBlocking();
    protos::gen::Trace trace;
    EXPECT_TRUE(trace.ParseFromArray(bytes.data(), bytes.size()));
    return TestPackets(trace);
  }

  static perfetto::TraceConfig MakeStallingConfig() {
    perfetto::TraceConfig cfg;
    auto* buffer = cfg.add_buffers();
    buffer->set_size_kb(1024);
    buffer->set_experimental_mode(
        perfetto::protos::gen::TraceConfig::BufferConfig::TRACE_BUFFER_V2);
    auto* ds_cfg = cfg.add_data_sources()->mutable_config();
    ds_cfg->set_name("tracing_v2_test");
    ds_cfg->set_use_tracing_v2(true);
    ds_cfg->set_buffer_exhausted_policy(
        perfetto::protos::gen::DataSourceConfig::
            BUFFER_EXHAUSTED_STALL_THEN_DROP);
    return cfg;
  }

  static protos::gen::Trace StopAndParse(perfetto::TracingSession* session) {
    session->StopBlocking();
    const std::vector<char> bytes = session->ReadTraceBlocking();
    protos::gen::Trace trace;
    EXPECT_TRUE(trace.ParseFromArray(bytes.data(), bytes.size()));
    return trace;
  }

  static std::vector<protos::gen::TracePacket> TestPackets(
      const protos::gen::Trace& trace) {
    std::vector<protos::gen::TracePacket> out;
    for (const protos::gen::TracePacket& packet : trace.packet()) {
      if (packet.has_for_testing())
        out.push_back(packet);
    }
    return out;
  }
  static void CheckFlushFifo(WriterSelection selection) {
    base::WaitableEvent older_flush_held;
    base::WaitableEvent later_flush_ran;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&older_flush_held);
    TracingV2FlushingDataSource::on_flush_done.store(&later_flush_ran);
    auto older =
        StartSession(MakeConfigFor({"tracing_v2_async_flush"}, selection));
    auto later =
        StartSession(MakeConfigFor({"tracing_v2_flushing"}, selection));

    std::mutex mutex;
    std::vector<std::string> completions;  // Guarded by |mutex|.
    bool older_succeeded = false;          // Guarded by |mutex|.
    bool later_succeeded = false;          // Guarded by |mutex|.
    base::WaitableEvent older_done;
    base::WaitableEvent later_done;

    older->Flush(
        [&](bool success) {
          std::lock_guard<std::mutex> lock(mutex);
          completions.push_back("older");
          older_succeeded = success;
          older_done.Notify();
        },
        /*timeout_ms=*/30000);
    // The older request is now inside its data source's OnFlush(), which will
    // not report completion until this test says so.
    older_flush_held.Wait();

    later->Flush(
        [&](bool success) {
          std::lock_guard<std::mutex> lock(mutex);
          completions.push_back("later");
          later_succeeded = success;
          later_done.Notify();
        },
        /*timeout_ms=*/30000);
    // The later request's OnFlush() is done, but its ring buffer barrier cannot
    // start while the older request remains at the front of the queue.
    later_flush_ran.Wait();

    // Wait for queued producer and service tasks before checking completion.
    perfetto::test::SyncProducers();
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_TRUE(completions.empty());
    }

    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    older_done.Wait();
    later_done.Wait();
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_EQ(completions, (std::vector<std::string>{"older", "later"}));
      EXPECT_TRUE(older_succeeded);
      EXPECT_TRUE(later_succeeded);
    }

    // The flush includes the packet written just before its callback completed.
    const auto packets = ReadTestPackets(older.get());
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].for_testing().str(), "async flush");
    older->StopBlocking();
    later->StopBlocking();
  }

  static void CheckStoppedFlush(WriterSelection selection) {
    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    auto older =
        StartSession(MakeConfigFor({"tracing_v2_async_flush"}, selection));
    std::atomic<bool> older_done{false};
    older->Flush(
        [&](bool success) {
          EXPECT_TRUE(success);
          older_done.store(true);
        },
        30000);
    held.Wait();
    older->StopBlocking();

    auto later =
        StartSession(MakeConfigFor({"tracing_v2_flushing"}, selection));
    EXPECT_TRUE(later->FlushBlocking(30000));
    // The held OnFlush() has not completed. Stopping the instance must release
    // the flush on its own.
    EXPECT_TRUE(older_done.load());
    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    WaitForMuxerSequence();
    EXPECT_TRUE(later->FlushBlocking(30000));
    later->StopBlocking();
  }
  static void CheckMuxerPressure(
      protos::gen::DataSourceConfig::BufferExhaustedPolicy policy) {
    auto cfg = MakeConfigFor({"tracing_v2_pressure"}, WriterSelection::kV2);
    cfg.mutable_data_sources()
        ->at(0)
        .mutable_config()
        ->set_buffer_exhausted_policy(policy);
    auto session = StartSession(cfg);
    ASSERT_TRUE(HasV2Connection());

    // OnFlush() writes 2 MiB on the muxer sequence without yielding to tasks.
    // ProducerEndpointImpl::NotifyTracingV2RingData drains inline on that
    // sequence. This must let the stalling writer complete without deadlock.
    base::WaitableEvent flushed;
    session->Flush(
        [&](bool success) {
          EXPECT_TRUE(success);
          flushed.Notify();
        },
        30000);
    flushed.Wait();
    WaitForMuxerSequence();
    if (policy ==
        protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_STALL_THEN_ABORT) {
      // Stalling makes progress via the inline drain, so nothing is dropped.
      EXPECT_EQ(TracingV2PressureDataSource::drops.load(), 0u);
    }
    // If v2 dropped data, its next chunk carries the loss flag and is
    // discarded. Flush that chunk before writing the tail so the tail can be
    // delivered.
    for (const char* message : {"resume after pressure", "after pressure"}) {
      base::WaitableEvent tail_flushed;
      Internals::PostToMuxerSequence([&] {
        TracingV2PressureDataSource::Trace(
            [&](TracingV2PressureDataSource::TraceContext ctx) {
              ctx.NewTracePacket()->set_for_testing()->set_str(message);
              ctx.Flush([&] { tail_flushed.Notify(); });
            });
      });
      tail_flushed.Wait();
    }
    session->StopBlocking();
    const auto packets = ReadTestPackets(session.get());
    bool saw_tail = false;
    for (const auto& packet : packets)
      saw_tail |= packet.for_testing().str() == "after pressure";
    // The tail written and flushed after the pressure episode must come
    // through: the ring recovered.
    EXPECT_TRUE(saw_tail);
  }
};

// A previous test may already have put the shared connection on v2. Check that
// selecting v1 keeps its instances on v1 and does not create a new v2
// connection where none existed.
TEST_F(TracingV2InProcessTest, V1ConfigDoesNotSelectV2) {
  const bool had_connection = HasV2Connection();
  auto absent = StartSession(
      MakeConfigFor({"tracing_v2_test"}, WriterSelection::kAbsent));
  auto off =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV1));

  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);
  EXPECT_EQ(HasV2Connection(), had_connection);

  StopAndParse(absent.get());
  StopAndParse(off.get());
}

// The check is on the config, before the interceptor name is even resolved.
TEST_F(TracingV2InProcessTest, TracingV2WithAnInterceptorIsFatal) {
  perfetto::TraceConfig cfg;
  auto* buffer = cfg.add_buffers();
  buffer->set_size_kb(1024);
  // Supply a valid TBv2 target. The interceptor must be the sole cause of the
  // abort on the muxer sequence.
  buffer->set_experimental_mode(
      perfetto::protos::gen::TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("tracing_v2_test");
  ds_cfg->set_use_tracing_v2(true);
  ds_cfg->mutable_interceptor_config()->set_name("unknown");

  // Setup aborts on the muxer sequence. Re-execute the test so the child starts
  // its own muxer thread. Fork alone leaves no thread to process setup.
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH_IF_SUPPORTED(StartSession(cfg),
                            "cannot be combined with an interceptor");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

// Interceptors without v2 keep working as before.
TEST_F(TracingV2InProcessTest, AnInterceptedInstanceWithoutTracingV2IsFine) {
  const bool had_connection = HasV2Connection();
  base::TempFile console_output = base::TempFile::Create();
  perfetto::ConsoleInterceptor::SetOutputFdForTesting(console_output.fd());

  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("tracing_v2_test");
  ds_cfg->mutable_interceptor_config()->set_name("console");
  auto session = StartSession(cfg);

  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);
  EXPECT_EQ(HasV2Connection(), had_connection);

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("intercepted");
    ctx.Flush();
  });

  // The packet went to the interceptor, not to the session's buffer.
  EXPECT_TRUE(TestPackets(StopAndParse(session.get())).empty());
  perfetto::ConsoleInterceptor::SetOutputFdForTesting(STDERR_FILENO);
}

TEST_F(TracingV2InProcessTest, V2ConnectionRejectsLaterStartupInstance) {
  const std::string previous_style = testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH_IF_SUPPORTED(
      {
        PutConnectionOnTracingV2();
        SetupStartupInstanceOnV2Connection();
      },
      "Startup tracing cannot share a producer connection");
  testing::GTEST_FLAG(death_test_style) = previous_style;
}

TEST_F(TracingV2InProcessTest, AV2ConfigCreatesTheRingOnDemand) {
  auto session =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 1u);
  EXPECT_TRUE(HasV2Connection());
  StopAndParse(session.get());
}

TEST_F(TracingV2InProcessTest, TheConfigSelectsTheWriterPerInstance) {
  auto absent = StartSession(
      MakeConfigFor({"tracing_v2_test"}, WriterSelection::kAbsent));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);

  auto off =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV1));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);

  auto on =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 1u);

  // One Trace() call writes to all three instances. Each must use the writer
  // selected by its config and deliver packets to its own session's buffer.
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("mixed");
    ctx.Flush();
  });

  for (perfetto::TracingSession* session :
       {absent.get(), off.get(), on.get()}) {
    const auto packets = TestPackets(StopAndParse(session));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].for_testing().str(), "mixed");
  }
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 0u);
}

class TracingV2ChunkSizeTest : public TracingV2InProcessTest {};

TEST_F(TracingV2ChunkSizeTest, ConfiguredSizeReachesRingAndRemainsFixed) {
  // The chunk count follows from the negotiated allocation budget. Cover
  // default, explicit, non-power-of-two and maximum chunk sizes.
  CheckConnectionSizing(/*requested=*/0, /*chunk=*/4096, /*num_chunks=*/32);
  CheckConnectionSizing(256, 256, 512);
  CheckConnectionSizing(260, 260, 512);
  CheckConnectionSizing(1024, 1024, 128);
  CheckConnectionSizing(32768, 32768, 4);

  auto cfg = MakeConfig();
  auto* producer_config = cfg.add_producers();
  producer_config->set_producer_name(
      Platform::GetDefaultPlatform()->GetCurrentProcessName());
  producer_config->set_tracing_v2_chunk_size_bytes(1024);
  auto session = StartSession(cfg);
  auto connection = GetConnection();
  ASSERT_NE(connection, nullptr);
  EXPECT_EQ(connection->ring->chunk_size(), 1024u);
  EXPECT_EQ(connection->ring->num_chunks(), 128u);
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str(std::string(4096, 'c'));
  });
  auto packets = TestPackets(StopAndParse(session.get()));
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), std::string(4096, 'c'));
  session.reset();

  // The connection outlives the session and its ring geometry is fixed: a
  // second session with a different configured chunk size reuses it unchanged.
  producer_config->set_tracing_v2_chunk_size_bytes(2048);
  session = StartSession(cfg);
  EXPECT_EQ(GetConnection(), connection);
  EXPECT_EQ(connection->ring->chunk_size(), 1024u);
  EXPECT_EQ(connection->ring->num_chunks(), 128u);
  StopAndParse(session.get());
}

TEST_F(TracingV2InProcessTest, EnabledProducesAValidTraceThroughTheRing) {
  auto session = StartSession(MakeConfig());
  auto connection = GetConnection();
  ASSERT_NE(connection, nullptr);
  EXPECT_EQ(connection->ring->chunk_size(), 4096u);

  for (uint32_t i = 0; i < 32; ++i) {
    TracingV2TestDataSource::Trace(
        [i](TracingV2TestDataSource::TraceContext ctx) {
          auto packet = ctx.NewTracePacket();
          packet->set_timestamp(i);
          auto* event = packet->set_for_testing();
          event->set_str("v2");
          // Exercise the proto group rewrite with a nested message.
          auto* payload = event->set_payload();
          payload->set_single_int(static_cast<int32_t>(i));
          payload->add_str("nested");
        });
  }
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const auto packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), 32u);
  for (uint32_t i = 0; i < 32; ++i) {
    EXPECT_EQ(packets[i].timestamp(), i) << i;
    EXPECT_EQ(packets[i].for_testing().str(), "v2") << i;
    EXPECT_EQ(packets[i].for_testing().payload().single_int(),
              static_cast<int32_t>(i))
        << i;
    ASSERT_EQ(packets[i].for_testing().payload().str().size(), 1u) << i;
    EXPECT_EQ(packets[i].for_testing().payload().str()[0], "nested") << i;
  }
}

TEST_F(TracingV2InProcessTest, FlushCallbackWaitsForTheService) {
  auto session = StartSession(MakeConfig());

  base::WaitableEvent flush_complete;
  TracingV2TestDataSource::Trace(
      [&flush_complete](TracingV2TestDataSource::TraceContext ctx) {
        {
          auto packet = ctx.NewTracePacket();
          packet->set_timestamp(1234);
          packet->set_for_testing()->set_str("flush");
        }
        ctx.Flush([&flush_complete] { flush_complete.Notify(); });
      });
  flush_complete.Wait();

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 1234u);
  session->StopBlocking();
}

TEST_F(TracingV2InProcessTest, MuxerStallWithBothBuffersFullMakesProgress) {
  CheckMuxerPressure(
      protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_STALL_THEN_ABORT);
}

TEST_F(TracingV2InProcessTest,
       MuxerStallThenDropWithBothBuffersFullMakesProgress) {
  CheckMuxerPressure(
      protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_STALL_THEN_DROP);
}

TEST_F(TracingV2InProcessTest, MuxerDropUnderPressureMakesProgress) {
  CheckMuxerPressure(protos::gen::DataSourceConfig::BUFFER_EXHAUSTED_DROP);
}

// Write a packet larger than the producer ring, then check that the writer
// makes progress and later packets reach the trace. If the large packet is
// delivered, its contents must match. Otherwise, the trace must report loss.
TEST_F(TracingV2InProcessTest, PacketLargerThanRingMakesProgressAndRecovers) {
  auto session = StartSession(MakeStallingConfig());

  // Establish the sequence before pressure: a loss marker on its first packet
  // could come from service initialization and would not prove recovery.
  base::WaitableEvent initial_flushed;
  TracingV2TestDataSource::Trace(
      [&](TracingV2TestDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("before large packet");
        ctx.Flush([&] { initial_flushed.Notify(); });
      });
  initial_flushed.Wait();

  // Exceeds the ring's 256 KiB allocation budget.
  const std::string payload(512 * 1024, 'z');
  TracingV2TestDataSource::Trace(
      [&payload](TracingV2TestDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(11);
        packet->set_for_testing()->set_str(payload);
      });
  base::WaitableEvent flushed;
  TracingV2TestDataSource::Trace(
      [&](TracingV2TestDataSource::TraceContext ctx) {
        ctx.Flush([&] { flushed.Notify(); });
      });
  flushed.Wait();
  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("after large packet");
  });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const auto packets = TestPackets(trace);
  ASSERT_GE(packets.size(), 2u);
  EXPECT_EQ(packets.front().for_testing().str(), "before large packet");
  EXPECT_EQ(packets.back().for_testing().str(), "after large packet");
  if (packets.size() == 3) {
    EXPECT_EQ(packets[1].timestamp(), 11u);
    EXPECT_EQ(packets[1].for_testing().str(), payload);
  } else {
    ASSERT_EQ(packets.size(), 2u);
    EXPECT_NE(packets[1].previous_packet_dropped() &
                  protos::gen::TracePacket::DATA_LOSS_PRESENT,
              0u);
  }

  // Delivered packets must not report reassembly errors. This check cannot
  // detect an error if the packet that carries its reason bits is lost.
  constexpr uint32_t kV2ReassemblyLoss =
      protos::gen::TracePacket::DATA_LOSS_ORPHAN_CONTINUATION |
      protos::gen::TracePacket::DATA_LOSS_REASSEMBLY_GAP |
      protos::gen::TracePacket::DATA_LOSS_CHUNK_CORRUPTED;
  for (const protos::gen::TracePacket& packet : trace.packet())
    EXPECT_EQ(packet.previous_packet_dropped() & kV2ReassemblyLoss, 0u);
}

TEST_F(TracingV2InProcessTest, SeveralThreadsWriteConcurrently) {
  auto session = StartSession(MakeConfig());

  constexpr uint32_t kNumThreads = 4;
  constexpr uint32_t kPacketsPerThread = 100;
  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([t] {
      for (uint32_t i = 0; i < kPacketsPerThread; ++i) {
        TracingV2TestDataSource::Trace(
            [t, i](TracingV2TestDataSource::TraceContext ctx) {
              auto packet = ctx.NewTracePacket();
              packet->set_timestamp(t * 1000 + i);
              packet->set_for_testing()->set_str("mt");
            });
      }
      // Each thread has its own TLS writer. Flush so its open chunk gets
      // published.
      TracingV2TestDataSource::Trace(
          [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });
    });
  }
  for (std::thread& thread : threads)
    thread.join();

  const protos::gen::Trace trace = StopAndParse(session.get());
  const auto packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), kNumThreads * kPacketsPerThread);
  // Per writer the order is guaranteed. Across writers it is not, so check the
  // set rather than the sequence.
  std::set<uint64_t> timestamps;
  for (const protos::gen::TracePacket& packet : packets)
    timestamps.insert(packet.timestamp());
  EXPECT_EQ(timestamps.size(), kNumThreads * kPacketsPerThread);
}

TEST_F(TracingV2InProcessTest, TwoSessionsWithDifferentBuffersStayApart) {
  auto first = StartSession(MakeConfig());
  auto second = StartSession(MakeConfig());
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2TestDataSource>(), 2u);

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    auto packet = ctx.NewTracePacket();
    packet->set_timestamp(42);
    packet->set_for_testing()->set_str("both");
  });
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  // Two instances of the data source use two writers with different target
  // buffers. Each session must see its own copy and nothing else.
  const protos::gen::Trace second_trace = StopAndParse(second.get());
  const protos::gen::Trace first_trace = StopAndParse(first.get());
  ASSERT_EQ(TestPackets(first_trace).size(), 1u);
  ASSERT_EQ(TestPackets(second_trace).size(), 1u);
  EXPECT_EQ(TestPackets(first_trace)[0].for_testing().str(), "both");
  EXPECT_EQ(TestPackets(second_trace)[0].for_testing().str(), "both");
}

// A v2 no_flush instance still participates in the producer flush protocol.
// The SDK skips OnFlush() and requests a service drain of the ring. The final
// packet must reach the trace before flush completion.
TEST_F(TracingV2InProcessTest,
       NoFlushDataSourceTailIsPresentWhenFlushCompletes) {
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_no_flush"}, WriterSelection::kV2));

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(1);
        packet->set_for_testing()->set_str("tail");
      });
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

  // Read before stopping: stopping would flush again on its own.
  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "tail");
  session->StopBlocking();
}

// A no_flush data source that stayed on v1 keeps its existing behaviour: the
// service never asks the producer, and the SMB scrape recovers the tail.
TEST_F(TracingV2InProcessTest, NoFlushDataSourceOnV1IsStillScraped) {
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_no_flush"}, WriterSelection::kV1));
  EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2NoFlushDataSource>(), 0u);

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(1);
        packet->set_for_testing()->set_str("scraped tail");
      });
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "scraped tail");
  session->StopBlocking();
}

TEST_F(TracingV2InProcessTest, FlushAndCloneSessionIncludesNoFlushTail) {
  perfetto::TraceConfig cfg =
      MakeConfigFor({"tracing_v2_no_flush"}, WriterSelection::kV2);
  cfg.set_unique_session_name("tracing_v2_no_flush_clone");
  auto session = StartSession(cfg);

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(2);
        packet->set_for_testing()->set_str("cloned tail");
      });

  // Cloning flushes the same set of instances a consumer flush would.
  auto clone = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  base::WaitableEvent cloned;
  bool clone_succeeded = false;
  perfetto::TracingSession::CloneTraceArgs clone_args;
  clone_args.unique_session_name = "tracing_v2_no_flush_clone";
  clone->CloneTrace(clone_args,
                    [&](perfetto::TracingSession::CloneTraceCallbackArgs args) {
                      clone_succeeded = args.success;
                      cloned.Notify();
                    });
  cloned.Wait();
  ASSERT_TRUE(clone_succeeded);

  const auto packets = ReadTestPackets(clone.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "cloned tail");
  session->StopBlocking();
}

// A consumer flush drains both v2 instances but calls OnFlush() only for the
// one that supports it. The completed trace must include both instances' data
// and the packet written by OnFlush().
TEST_F(TracingV2InProcessTest, DeclaredNoFlushDataSourceGetsNoOnFlushCallback) {
  auto session = StartSession(
      MakeConfigFor({"tracing_v2_flushing", "tracing_v2_declared_no_flush"},
                    WriterSelection::kV2));

  TracingV2FlushingDataSource::Trace(
      [](TracingV2FlushingDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("flushing");
      });
  TracingV2DeclaredNoFlushDataSource::Trace(
      [](TracingV2DeclaredNoFlushDataSource::TraceContext ctx) {
        ctx.NewTracePacket()->set_for_testing()->set_str("declared");
      });

  const uint32_t flushing_before = TracingV2FlushingDataSource::flushes;
  const uint32_t declared_before = TracingV2DeclaredNoFlushDataSource::flushes;
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));
  EXPECT_EQ(TracingV2FlushingDataSource::flushes - flushing_before, 1u);
  EXPECT_EQ(TracingV2DeclaredNoFlushDataSource::flushes - declared_before, 0u);

  std::vector<std::string> strings;
  for (const protos::gen::TracePacket& packet : ReadTestPackets(session.get()))
    strings.push_back(packet.for_testing().str());
  std::sort(strings.begin(), strings.end());
  EXPECT_EQ(strings,
            (std::vector<std::string>{"declared", "flushing", "on flush"}));
  session->StopBlocking();
}

// A synchronous flush must wait for an older asynchronous OnFlush() to finish.
// Otherwise its cumulative ack would complete the older flush before its final
// packets were written.
TEST_F(TracingV2InProcessTest, LaterFlushDoesNotAcknowledgeAnOlderOne) {
  CheckFlushFifo(WriterSelection::kV2);
}

// On a v2 connection, v1-only requests also wait in the ordered queue.
// A consumer timeout leaves the producer's callback pending, so later requests
// wait until it completes or its data source stops.
// NeverV2ConnectionKeepsV1FlushAcks covers connections that stay on v1.
TEST_F(TracingV2InProcessTest, TimedOutFlushStillBlocksLaterProducerRequests) {
  RunInFreshProcess([] {
    auto older = StartSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV2));
    auto newer = StartSession(
        MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV2));
    base::WaitableEvent held;
    base::WaitableEvent timed_out;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    older->Flush(
        [&](bool success) {
          EXPECT_FALSE(success);
          timed_out.Notify();
        },
        /*timeout_ms=*/1);
    held.Wait();
    timed_out.Wait();
    EXPECT_EQ(PendingFlushCount(), 1u);

    base::WaitableEvent newer_handled;
    base::WaitableEvent newer_completed;
    std::atomic<bool> acknowledged{false};
    TracingV2FlushingDataSource::on_flush_done.store(&newer_handled);
    newer->Flush(
        [&](bool success) {
          EXPECT_TRUE(success);
          acknowledged.store(true);
          newer_completed.Notify();
        },
        /*timeout_ms=*/30000);
    newer_handled.Wait();
    WaitForMuxerSequence();
    EXPECT_EQ(PendingFlushCount(), 2u);
    EXPECT_FALSE(acknowledged.load());
    TracingV2FlushingDataSource::on_flush_done.store(nullptr);

    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    newer_completed.Wait();
    EXPECT_EQ(PendingFlushCount(), 0u);
    newer->StopBlocking();
    older->StopBlocking();
  });
}

TEST_F(TracingV2InProcessTest, V1FlushesOnAV2ConnectionAreFifo) {
  PutConnectionOnTracingV2();
  CheckFlushFifo(WriterSelection::kV1);
}

TEST_F(TracingV2InProcessTest, StoppedAsyncFlushDoesNotBlockLaterFlush) {
  CheckStoppedFlush(WriterSelection::kV2);
}

// An older flush can await a v1 instance's callback while a v2 flush waits
// behind it. Stopping the v1 instance must release the older request.
TEST_F(TracingV2InProcessTest,
       StoppedV1AsyncFlushOnAV2ConnectionDoesNotBlockLaterFlush) {
  PutConnectionOnTracingV2();
  CheckStoppedFlush(WriterSelection::kV1);
}

// A v1-only flush must respect the connection's queue:
// 1. Hold an older v2 flush inside its asynchronous OnFlush() callback.
// 2. Submit a v1-only flush and let its OnFlush() callback complete.
// 3. Verify that its acknowledgement waits for the older v2 flush.
TEST_F(TracingV2InProcessTest, V1FlushOnAV2ConnectionWaitsBehindAV2Barrier) {
  auto v2 = StartSession(
      MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV2));
  auto v1 = StartSession(
      MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV1));

  base::WaitableEvent v2_flush_held;
  TracingV2AsyncFlushDataSource::hold_next_flush.store(&v2_flush_held);

  std::mutex mutex;
  std::vector<std::string> completions;  // Guarded by |mutex|.
  base::WaitableEvent v2_done;
  base::WaitableEvent v1_done;
  v2->Flush(
      [&](bool success) {
        EXPECT_TRUE(success);
        std::lock_guard<std::mutex> lock(mutex);
        completions.push_back("v2");
        v2_done.Notify();
      },
      /*timeout_ms=*/30000);
  // The older v2 request is now parked inside its OnFlush() at the front of the
  // queue.
  v2_flush_held.Wait();
  v1->Flush(
      [&](bool success) {
        EXPECT_TRUE(success);
        std::lock_guard<std::mutex> lock(mutex);
        completions.push_back("v1");
        v1_done.Notify();
      },
      /*timeout_ms=*/30000);
  // The later v1 request cannot be acked while the older v2 request is at the
  // front of the queue.
  perfetto::test::SyncProducers();
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_TRUE(completions.empty());
  }

  TracingV2AsyncFlushDataSource::CompleteHeldFlush();
  v2_done.Wait();
  v1_done.Wait();
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(completions, (std::vector<std::string>{"v2", "v1"}));
  }
  v2->StopBlocking();
  v1->StopBlocking();
}

// An older service can flush a v1 no_flush instance. Preserve its OnFlush().
// Only v2 instances skip that callback when the service requests a drain.
// DeclaredNoFlushDataSourceGetsNoOnFlushCallback covers the v2 case.
TEST_F(TracingV2InProcessTest,
       OlderServiceFlushOfAV1NoFlushInstanceCallsOnFlush) {
  // Acks are cumulative: NotifyFlushComplete(N) completes every request of
  // this producer up to N. The synthetic id must therefore be below any real
  // one. The service numbers requests from 1, so an ack for 0 completes
  // nothing.
  constexpr FlushRequestID kFlushIdBelowAnyReal = 0;
  for (WriterSelection selection :
       {WriterSelection::kV1, WriterSelection::kV2}) {
    auto session = StartSession(
        MakeConfigFor({"tracing_v2_declared_no_flush"}, selection));
    const uint32_t before = TracingV2DeclaredNoFlushDataSource::flushes;
    FlushInstanceLikeAnOlderService<TracingV2DeclaredNoFlushDataSource>(
        kFlushIdBelowAnyReal);
    const uint32_t expected_calls = selection == WriterSelection::kV1 ? 1 : 0;
    EXPECT_EQ(TracingV2DeclaredNoFlushDataSource::flushes - before,
              expected_calls);
    session->StopBlocking();
    // The instance slot must be free before the next iteration looks it up.
    WaitForMuxerSequence();
  }
}

// The stop path must drain the ring before acknowledgement.
// A packet without an explicit flush must therefore reach the trace on stop.
TEST_F(TracingV2InProcessTest, StopDeliversWhatWasStillInTheRing) {
  auto session =
      StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    ctx.NewTracePacket()->set_for_testing()->set_str("stop tail");
  });

  // No explicit flush: only the stop path can move this packet out of the ring.
  session->StopBlocking();

  const auto packets = ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "stop tail");
}

// A process that never selects v2 creates no producer ring. Its writers come
// from the endpoint. Both synchronous and asynchronous flushes complete
// through the v1 path, as does Shutdown().
TEST_F(TracingV2InProcessTest, NeverSelectingV2StaysOnV1ThroughShutdown) {
  RunInFreshProcess([] {
    EXPECT_FALSE(HasV2Connection());
    auto session = StartSession(
        MakeConfigFor({"tracing_v2_flushing", "tracing_v2_async_flush"},
                      WriterSelection::kV1));
    TracingV2FlushingDataSource::Trace(
        [](TracingV2FlushingDataSource::TraceContext ctx) {
          ctx.NewTracePacket()->set_for_testing()->set_str("v1");
        });
    EXPECT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    base::WaitableEvent flushed;
    std::atomic<bool> flush_succeeded{false};
    session->Flush(
        [&](bool success) {
          flush_succeeded.store(success);
          flushed.Notify();
        },
        /*timeout_ms=*/30000);
    held.Wait();
    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    flushed.Wait();
    EXPECT_TRUE(flush_succeeded.load());

    // Each of the two flushes ran the flushing data source's OnFlush() once.
    std::vector<std::string> strings;
    for (const auto& packet : ReadTestPackets(session.get()))
      strings.push_back(packet.for_testing().str());
    std::sort(strings.begin(), strings.end());
    EXPECT_EQ(strings, (std::vector<std::string>{"async flush", "on flush",
                                                 "on flush", "v1"}));
    session->StopBlocking();
    EXPECT_EQ(CountInstancesUsingTracingV2<TracingV2FlushingDataSource>(), 0u);
    EXPECT_FALSE(HasV2Connection());

    // Shutdown() needs the consumer gone.
    session.reset();
    WaitForMuxerSequence();
    perfetto::Tracing::Shutdown();
  });
}

// V1 can leave a flush waiting for a stopped instance. Selecting v2 removes
// that wait and acks the completed request immediately.
TEST_F(TracingV2InProcessTest,
       StoppedV1AsyncFlushBeforeSelectingV2DoesNotBlockV2Flushes) {
  RunInFreshProcess([] {
    auto older = StartSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));
    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    older->Flush([](bool) {}, /*timeout_ms=*/30000);
    held.Wait();
    TracingV2AsyncFlushDataSource::hold_next_flush.store(nullptr);
    older->StopBlocking();
    EXPECT_EQ(PendingFlushCount(), 1u);

    auto later =
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
    EXPECT_EQ(PendingFlushCount(), 0u);
    EXPECT_TRUE(later->FlushBlocking(/*timeout_ms=*/5000));
    later->StopBlocking();
    // Runs the stale completion, which finds its instance gone.
    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    WaitForMuxerSequence();
  });
}

// A synchronous v1 flush also acks the older request held in OnFlush().
// Remove that request before switching to v2 so it cannot block the queue.
// Ignore its late callback.
TEST_F(TracingV2InProcessTest,
       CumulativeV1AckBeforeSelectingV2DoesNotBlockV2Flushes) {
  RunInFreshProcess([] {
    auto older = StartSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));
    auto newer = StartSession(
        MakeConfigFor({"tracing_v2_flushing"}, WriterSelection::kV1));
    base::WaitableEvent held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&held);
    base::WaitableEvent older_flushed;
    std::atomic<bool> older_succeeded{false};
    older->Flush(
        [&](bool success) {
          older_succeeded.store(success);
          older_flushed.Notify();
        },
        /*timeout_ms=*/30000);
    held.Wait();
    EXPECT_EQ(PendingFlushCount(), 1u);

    EXPECT_TRUE(newer->FlushBlocking(/*timeout_ms=*/30000));
    older_flushed.Wait();
    EXPECT_TRUE(older_succeeded.load());
    EXPECT_EQ(PendingFlushCount(), 0u);

    auto v2 =
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
    EXPECT_TRUE(v2->FlushBlocking(/*timeout_ms=*/5000));

    TracingV2AsyncFlushDataSource::CompleteHeldFlush();
    WaitForMuxerSequence();
    EXPECT_EQ(PendingFlushCount(), 0u);
    v2->StopBlocking();
    newer->StopBlocking();
    older->StopBlocking();
  });
}

// Verify delivery before shutdown after v2 use. In this in-process test, the
// service drains on the muxer sequence. Shutdown() uses the common teardown
// path because direct transport has no separate relay runner to destroy.
TEST_F(TracingV2InProcessTest, AcceptedDrainDoesNotCompleteAfterReset) {
  RunInFreshProcess([] {
    std::atomic<bool> completed{false};
    auto reply = HoldDrain([&] { completed.store(true); });
    ASSERT_TRUE(reply);
    TearDownTestSuite();
    SetUpTestSuite();
    reply(true);
    WaitForMuxerSequence();
    EXPECT_FALSE(completed.load());
    auto session = StartSession(MakeConfig());
    EXPECT_TRUE(session->FlushBlocking());
    session->StopBlocking();
  });
}

TEST_F(TracingV2InProcessTest, ShutdownAfterV2Use) {
  RunInFreshProcess([] {
    auto session =
        StartSession(MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2));
    ASSERT_TRUE(HasV2Connection());
    TracingV2TestDataSource::Trace(
        [](TracingV2TestDataSource::TraceContext ctx) {
          ctx.NewTracePacket()->set_for_testing()->set_str("shutdown");
          ctx.Flush();
        });
    const auto packets = TestPackets(StopAndParse(session.get()));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].for_testing().str(), "shutdown");

    // Shutdown() needs the consumer gone.
    session.reset();
    WaitForMuxerSequence();
    perfetto::Tracing::Shutdown();
  });
}

// Real endpoints batch flush acks into CommitData, which can hide a muxer
// ordering bug. This endpoint records and sends acks directly so the test can
// check that the muxer posts packet commits first.
//
// Its arbiter shares the wrapped endpoint's SMB and routes commits through
// this wrapper. The wrapped endpoint's arbiter only forwards acks.
class DirectAckProducerEndpoint : public ProxyProducerEndpoint {
 public:
  DirectAckProducerEndpoint(std::unique_ptr<ProducerEndpoint> wrapped,
                            base::TaskRunner* task_runner,
                            std::mutex* log_mutex,
                            std::vector<std::string>* log)
      : wrapped_(std::move(wrapped)),
        task_runner_(task_runner),
        log_mutex_(log_mutex),
        log_(log) {
    set_backend(wrapped_.get());
  }

  // OnTracingSetup() first calls this on the muxer thread, before any writer
  // exists. Later calls only read the initialized arbiter pointer.
  SharedMemoryArbiter* MaybeSharedMemoryArbiter() override {
    if (!arbiter_) {
      SharedMemory* shm = backend()->shared_memory();
      if (!shm)
        return nullptr;
      arbiter_ = SharedMemoryArbiter::CreateInstance(
          shm, backend()->shared_buffer_page_size_kb() * 1024,
          SharedMemoryABI::ShmemMode::kDefault, this, task_runner_);
      arbiter_->SetDirectSMBPatchingSupportedByService();
    }
    return arbiter_.get();
  }

  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer,
      BufferExhaustedPolicy buffer_exhausted_policy) override {
    return MaybeSharedMemoryArbiter()->CreateTraceWriter(
        target_buffer, buffer_exhausted_policy);
  }

  void CommitData(const CommitDataRequest& req,
                  CommitDataCallback callback) override {
    if (req.chunks_to_move_size() > 0)
      Log("commit");
    ProxyProducerEndpoint::CommitData(req, std::move(callback));
  }

  void NotifyFlushComplete(FlushRequestID id) override {
    Log("ack " + std::to_string(id));
    MaybeSharedMemoryArbiter()->NotifyFlushComplete(id);
  }

 private:
  void Log(std::string entry) {
    std::lock_guard<std::mutex> lock(*log_mutex_);
    log_->push_back(std::move(entry));
  }

  std::unique_ptr<ProducerEndpoint> wrapped_;
  base::TaskRunner* const task_runner_;
  std::mutex* const log_mutex_;
  std::vector<std::string>* const log_;
  std::unique_ptr<SharedMemoryArbiter> arbiter_;
};

// The in-process backend, with the producer side wrapped as above.
class DirectAckBackend : public TracingBackend {
 public:
  std::unique_ptr<ProducerEndpoint> ConnectProducer(
      const ConnectProducerArgs& args) override {
    return std::make_unique<DirectAckProducerEndpoint>(
        InProcessTracingBackend::GetInstance()->ConnectProducer(args),
        args.task_runner, &log_mutex_, &log_);
  }

  std::unique_ptr<ConsumerEndpoint> ConnectConsumer(
      const ConnectConsumerArgs& args) override {
    return InProcessTracingBackend::GetInstance()->ConnectConsumer(args);
  }

  std::vector<std::string> TakeLog() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return std::move(log_);
  }

 private:
  std::mutex log_mutex_;
  std::vector<std::string> log_;  // Guarded by |log_mutex_|.
};

// Runs each test in a fresh process that it initializes itself, on
// DirectAckBackend.
class TracingV2DirectAckTest : public TracingV2InProcessTest {
 protected:
  static void SetUpTestSuite() {}
  static void TearDownTestSuite() {}

  static std::unique_ptr<perfetto::TracingSession> StartCustomBackendSession(
      const perfetto::TraceConfig& cfg) {
    auto session = perfetto::Tracing::NewTrace(perfetto::kCustomBackend);
    session->Setup(cfg);
    session->StartBlocking();
    return session;
  }
};

// Stopping a v1 instance on a v2 connection can release a waiting flush.
// Batch its final packets in the arbiter and check they are committed before
// that flush is acknowledged.
TEST_F(TracingV2DirectAckTest, StoppedV1InstanceCommitsBeforeItsFlushIsAcked) {
  RunInFreshProcess([] {
    // Retain the backend until process exit because the muxer still uses it.
    auto* backend = new DirectAckBackend();
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kCustomBackend;
    args.custom_backend = backend;
    perfetto::Tracing::Initialize(args);
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name("tracing_v2_test");
    TracingV2TestDataSource::Register(dsd);
    dsd.set_name("tracing_v2_async_flush");
    TracingV2AsyncFlushDataSource::Register(dsd);
    perfetto::test::SyncProducers();

    // Put the connection on v2, then keep completed chunks queued in the
    // arbiter instead of sending them right away.
    StartCustomBackendSession(
        MakeConfigFor({"tracing_v2_test"}, WriterSelection::kV2))
        ->StopBlocking();
    WaitForMuxerSequence();
    auto session = StartCustomBackendSession(
        MakeConfigFor({"tracing_v2_async_flush"}, WriterSelection::kV1));
    perfetto::test::SetBatchCommitsDuration(60000, perfetto::kCustomBackend);
    base::WaitableEvent flush_held;
    TracingV2AsyncFlushDataSource::hold_next_flush.store(&flush_held);
    session->Flush([](bool) {}, /*timeout_ms=*/30000);
    flush_held.Wait();

    base::WaitableEvent stop_started;
    TracingV2AsyncStopDataSource::stop_started.store(&stop_started);
    session->Stop();
    stop_started.Wait();
    // Ignore setup traffic. From here on, record the packet commit and ack.
    backend->TakeLog();

    // The instance is still up while its stop is held. Write more than a
    // chunk, so that at least one completed chunk is queued for commit.
    TracingV2AsyncFlushDataSource::Trace(
        [](TracingV2AsyncFlushDataSource::TraceContext ctx) {
          ctx.NewTracePacket()->set_for_testing()->set_str(
              std::string(16 * 1024, 'd'));
        });

    // Completing the stop drops the instance from the pending flush, which
    // acks it.
    TracingV2AsyncStopDataSource::CompleteHeldStop();
    WaitForMuxerSequence();
    const std::vector<std::string> log = backend->TakeLog();
    EXPECT_THAT(log, testing::ElementsAre("commit", "ack 1"));
  });
}

TEST(TracingV2StartupTest, ExplicitSelectionFailsLoudly) {
  if (!PERFETTO_BUILDFLAG(PERFETTO_IPC))
    GTEST_SKIP() << "Startup tracing requires the system backend";
  const std::string previous_death_test_style =
      testing::GTEST_FLAG(death_test_style);
  testing::GTEST_FLAG(death_test_style) = "threadsafe";
  EXPECT_DEATH(
      {
        TracingInitArgs args;
        args.backends = kSystemBackend;
        Tracing::Initialize(args);
        DataSourceDescriptor descriptor;
        descriptor.set_name("startup_v2");
        TracingV2TestDataSource::Register(descriptor);
        TraceConfig cfg;
        cfg.add_buffers()->set_size_kb(1024);
        auto* ds = cfg.add_data_sources()->mutable_config();
        ds->set_name("startup_v2");
        ds->set_use_tracing_v2(true);
        Tracing::SetupStartupTracingOpts opts;
        opts.backend = kSystemBackend;
        Tracing::SetupStartupTracingBlocking(cfg, std::move(opts));
      },
      "not supported for startup tracing");
  testing::GTEST_FLAG(death_test_style) = previous_death_test_style;
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
