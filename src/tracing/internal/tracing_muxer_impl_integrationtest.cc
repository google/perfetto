#include "perfetto/tracing/tracing.h"

#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <optional>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "perfetto/tracing/backend_type.h"
#include "perfetto/tracing/data_source.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trigger.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/tracing/internal/tracing_muxer_impl.h"
#include "src/tracing/test/api_test_support.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace internal {
namespace {

using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Property;

class TracingMuxerImplIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    GTEST_SKIP() << "Unix sockets not supported on windows";
#endif
  }

  // Sets the environment variable `name` to `value`. Restores it to the
  // previous value when the test finishes.
  void SetEnvVar(const char* name, const char* value) {
    prev_state_.emplace();
    EnvVar& var = prev_state_.top();
    var.name = name;
    const char* prev_value = getenv(name);
    if (prev_value) {
      var.value.emplace(prev_value);
    }
    base::SetEnv(name, value);
  }

  ~TracingMuxerImplIntegrationTest() override {
    perfetto::Tracing::ResetForTesting();
    while (!prev_state_.empty()) {
      const EnvVar& var = prev_state_.top();
      if (var.value) {
        base::SetEnv(var.name, *var.value);
      } else {
        base::UnsetEnv(var.name);
      }
      prev_state_.pop();
    }
  }

  struct EnvVar {
    const char* name;
    std::optional<std::string> value;
  };
  // Stores previous values of environment variables overridden by tests. We
  // need to to this because some android integration tests need to talk to the
  // real system tracing service and need the PERFETTO_PRODUCER_SOCK_NAME and
  // PERFETTO_CONSUMER_SOCK_NAME to be set to their original value.
  std::stack<EnvVar> prev_state_;
};

class TracingServiceThread {
 public:
  TracingServiceThread(const std::string& producer_socket,
                       const std::string& consumer_socket)
      : runner_(base::ThreadTaskRunner::CreateAndStart("perfetto.svc")),
        producer_socket_(producer_socket),
        consumer_socket_(consumer_socket) {
    runner_.PostTaskAndWaitForTesting([this]() {
      svc_ = ServiceIPCHost::CreateInstance(&runner_);
      bool res =
          svc_->Start(producer_socket_.c_str(), consumer_socket_.c_str());
      if (!res) {
        PERFETTO_FATAL("Failed to start service listening on %s and %s",
                       producer_socket_.c_str(), consumer_socket_.c_str());
      }
    });
  }

  ~TracingServiceThread() {
    runner_.PostTaskAndWaitForTesting([this]() { svc_.reset(); });
  }

  const std::string& producer_socket() const { return producer_socket_; }
  const std::string& consumer_socket() const { return consumer_socket_; }

 private:
  base::ThreadTaskRunner runner_;

  std::string producer_socket_;
  std::string consumer_socket_;
  std::unique_ptr<ServiceIPCHost> svc_;
};

TEST_F(TracingMuxerImplIntegrationTest, ActivateTriggers) {
  base::TmpDirTree tmpdir_;

  base::TestTaskRunner task_runner;

  ASSERT_FALSE(perfetto::Tracing::IsInitialized());

  tmpdir_.TrackFile("producer2.sock");
  tmpdir_.TrackFile("consumer.sock");
  TracingServiceThread tracing_service(tmpdir_.AbsolutePath("producer2.sock"),
                                       tmpdir_.AbsolutePath("consumer.sock"));
  // Instead of being a unix socket, producer.sock is a regular empty file.
  tmpdir_.AddFile("producer.sock", "");

  // Wrong producer socket: the producer won't connect yet, but the consumer
  // will.
  SetEnvVar("PERFETTO_PRODUCER_SOCK_NAME",
            tmpdir_.AbsolutePath("producer.sock").c_str());
  SetEnvVar("PERFETTO_CONSUMER_SOCK_NAME",
            tmpdir_.AbsolutePath("consumer.sock").c_str());

  TracingInitArgs args;
  args.backends = perfetto::kSystemBackend;
  perfetto::Tracing::Initialize(args);

  // TracingMuxerImpl::ActivateTriggers will be called without the producer side
  // of the service being connected. It should store the trigger for 10000ms.
  perfetto::Tracing::ActivateTriggers({"trigger2", "trigger1"}, 10000);

  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  perfetto::TraceConfig::TriggerConfig* tr_cfg = cfg.mutable_trigger_config();
  tr_cfg->set_trigger_mode(perfetto::TraceConfig::TriggerConfig::STOP_TRACING);
  tr_cfg->set_trigger_timeout_ms(10000);
  perfetto::TraceConfig::TriggerConfig::Trigger* trigger =
      tr_cfg->add_triggers();
  trigger->set_name("trigger1");

  std::unique_ptr<TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kSystemBackend);
  base::WaitableEvent on_stop;
  session->SetOnStopCallback([&on_stop] { on_stop.Notify(); });
  session->Setup(cfg);

  session->StartBlocking();

  // Swap producer.sock and producer2.sock. Now the client should connect to the
  // tracing service as a producer.
  ASSERT_EQ(rename(tmpdir_.AbsolutePath("producer2.sock").c_str(),
                   tmpdir_.AbsolutePath("producer3.sock").c_str()),
            0);
  ASSERT_EQ(rename(tmpdir_.AbsolutePath("producer.sock").c_str(),
                   tmpdir_.AbsolutePath("producer2.sock").c_str()),
            0);
  ASSERT_EQ(rename(tmpdir_.AbsolutePath("producer3.sock").c_str(),
                   tmpdir_.AbsolutePath("producer.sock").c_str()),
            0);

  on_stop.Wait();

  std::vector<char> bytes = session->ReadTraceBlocking();
  perfetto::protos::gen::Trace parsed_trace;
  ASSERT_TRUE(parsed_trace.ParseFromArray(bytes.data(), bytes.size()));
  EXPECT_THAT(
      parsed_trace,
      Property(&perfetto::protos::gen::Trace::packet,
               Contains(Property(
                   &perfetto::protos::gen::TracePacket::trigger,
                   Property(&perfetto::protos::gen::Trigger::trigger_name,
                            "trigger1")))));
}

// Tracing v2 in-process path. The resulting trace looks like v1, so tests also
// check the number of v2 writers created.

class TracingV2TestDataSource
    : public perfetto::DataSource<TracingV2TestDataSource> {
 public:
  constexpr static bool kBufferExhaustedPolicyConfigurable = true;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// No OnFlush() override, so Register() marks it no_flush.
class TracingV2NoFlushDataSource
    : public perfetto::DataSource<TracingV2NoFlushDataSource> {
 public:
  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// Counts OnFlush() calls.
class TracingV2FlushingDataSource
    : public perfetto::DataSource<TracingV2FlushingDataSource> {
 public:
  static std::atomic<uint32_t> flushes;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
  void OnFlush(const FlushArgs&) override { ++flushes; }
};
std::atomic<uint32_t> TracingV2FlushingDataSource::flushes{0};

// Handles OnFlush() but registers with no_flush set: it must not be asked.
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

// Repeated Initialize()/ResetForTesting() cycles fail in the existing fake
// backend fixture, with or without v2. Initialize this suite once.
class TracingV2InProcessTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (!perfetto::tracing_v2::SharedRingBuffer::SupportsWriterWait())
      return;
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    args.enable_tracing_v2 = true;
    perfetto::Tracing::Initialize(args);
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name("tracing_v2_test");
    TracingV2TestDataSource::Register(dsd);
    dsd.set_name("tracing_v2_no_flush");
    TracingV2NoFlushDataSource::Register(dsd);
    dsd.set_name("tracing_v2_flushing");
    TracingV2FlushingDataSource::Register(dsd);
    dsd.set_name("tracing_v2_declared_no_flush");
    dsd.set_no_flush(true);
    TracingV2DeclaredNoFlushDataSource::Register(dsd);
    perfetto::test::SyncProducers();
    perfetto::test::DisableReconnectLimit();
  }

  static void TearDownTestSuite() {
    if (!perfetto::tracing_v2::SharedRingBuffer::SupportsWriterWait())
      return;
    // The data source's thread-local state points at muxer state that
    // ResetForTesting() is about to destroy.
    perfetto::test::TracingMuxerImplInternalsForTest::
        ClearDataSourceTlsStateOnReset<TracingV2TestDataSource>();
    perfetto::test::TracingMuxerImplInternalsForTest::
        ClearDataSourceTlsStateOnReset<TracingV2NoFlushDataSource>();
    perfetto::test::TracingMuxerImplInternalsForTest::
        ClearDataSourceTlsStateOnReset<TracingV2FlushingDataSource>();
    perfetto::test::TracingMuxerImplInternalsForTest::
        ClearDataSourceTlsStateOnReset<TracingV2DeclaredNoFlushDataSource>();
    perfetto::Tracing::ResetForTesting();
  }

  void SetUp() override {
    if (!perfetto::tracing_v2::SharedRingBuffer::SupportsWriterWait())
      GTEST_SKIP() << "The tracing v2 ring needs a futex on this platform";
  }

  static perfetto::TraceConfig MakeConfig() {
    return MakeConfigFor({"tracing_v2_test"});
  }

  static perfetto::TraceConfig MakeConfigFor(
      std::initializer_list<const char*> data_source_names) {
    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(1024);
    for (const char* name : data_source_names)
      cfg.add_data_sources()->mutable_config()->set_name(name);
    return cfg;
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
    cfg.add_buffers()->set_size_kb(1024);
    perfetto::protos::gen::DataSourceConfig* ds_cfg =
        cfg.add_data_sources()->mutable_config();
    ds_cfg->set_name("tracing_v2_test");
    ds_cfg->set_buffer_exhausted_policy(
        perfetto::protos::gen::DataSourceConfig::
            BUFFER_EXHAUSTED_STALL_THEN_DROP);
    return cfg;
  }

  static protos::gen::Trace StopAndParse(perfetto::TracingSession* session) {
    session->StopBlocking();
    const std::vector<char> bytes = session->ReadTraceBlocking();
    // Used by the local end-to-end test with trace_processor_shell.
    if (const char* path = getenv("PERFETTO_TRACING_V2_TRACE_OUT")) {
      base::ScopedFstream f(fopen(path, "wb"));
      EXPECT_TRUE(f);
      if (f)
        EXPECT_EQ(fwrite(bytes.data(), 1, bytes.size(), *f), bytes.size());
    }
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
};

TEST_F(TracingV2InProcessTest, EnabledProducesAValidTraceThroughTheRing) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfig());
  session->StartBlocking();

  for (uint32_t i = 0; i < 32; ++i) {
    TracingV2TestDataSource::Trace(
        [i](TracingV2TestDataSource::TraceContext ctx) {
          auto packet = ctx.NewTracePacket();
          packet->set_timestamp(i);
          auto* event = packet->set_for_testing();
          event->set_str("v2");
          // Exercise the proto-group rewrite with a nested message.
          auto* payload = event->set_payload();
          payload->set_single_int(static_cast<int32_t>(i));
          payload->add_str("nested");
        });
  }
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const std::vector<protos::gen::TracePacket> packets = TestPackets(trace);
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
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfig());
  session->StartBlocking();

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

  const protos::gen::Trace trace = StopAndParse(session.get());
  const std::vector<protos::gen::TracePacket> packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 1234u);
}

// The packet is twice the size of the ring. A stalling writer therefore needs
// the relay to free chunks while the packet is still being encoded.
TEST_F(TracingV2InProcessTest,
       PacketLargerThanTheWholeRingReconstructsExactly) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeStallingConfig());
  session->StartBlocking();

  // Twice the 256 KiB ring.
  const std::string payload(512 * 1024, 'z');
  TracingV2TestDataSource::Trace(
      [&payload](TracingV2TestDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(11);
        packet->set_for_testing()->set_str(payload);
      });
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const std::vector<protos::gen::TracePacket> packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 11u);
  EXPECT_EQ(packets[0].for_testing().str(), payload);

  // Ignore loss attributed to the temporary v1 forwarding; the v2 reassembly
  // must remain intact.
  constexpr uint32_t kV2ReassemblyLoss =
      protos::gen::TracePacket::DATA_LOSS_ORPHAN_CONTINUATION |
      protos::gen::TracePacket::DATA_LOSS_REASSEMBLY_GAP |
      protos::gen::TracePacket::DATA_LOSS_CHUNK_CORRUPTED;
  for (const protos::gen::TracePacket& packet : trace.packet())
    EXPECT_EQ(packet.previous_packet_dropped() & kV2ReassemblyLoss, 0u);
}

TEST_F(TracingV2InProcessTest, SeveralThreadsWriteConcurrently) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfig());
  session->StartBlocking();

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
      // Each thread has its own TLS writer; flushing publishes whatever it is
      // holding so nothing stays in a chunk the reader cannot resolve.
      TracingV2TestDataSource::Trace(
          [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });
    });
  }
  for (std::thread& thread : threads)
    thread.join();

  const protos::gen::Trace trace = StopAndParse(session.get());
  const std::vector<protos::gen::TracePacket> packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), kNumThreads * kPacketsPerThread);
  // Per writer the order is guaranteed; across writers it is not, so check the
  // set rather than the sequence.
  std::set<uint64_t> timestamps;
  for (const protos::gen::TracePacket& packet : packets)
    timestamps.insert(packet.timestamp());
  EXPECT_EQ(timestamps.size(), kNumThreads * kPacketsPerThread);
}

TEST_F(TracingV2InProcessTest, TwoSessionsWithDifferentBuffersStayApart) {
  std::unique_ptr<perfetto::TracingSession> first =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  first->Setup(MakeConfig());
  first->StartBlocking();
  std::unique_ptr<perfetto::TracingSession> second =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  second->Setup(MakeConfig());
  second->StartBlocking();

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    auto packet = ctx.NewTracePacket();
    packet->set_timestamp(42);
    packet->set_for_testing()->set_str("both");
  });
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  // Two instances of the data source means two writers with different target
  // buffers; each session must see its own copy and nothing else.
  const protos::gen::Trace second_trace = StopAndParse(second.get());
  const protos::gen::Trace first_trace = StopAndParse(first.get());
  ASSERT_EQ(TestPackets(first_trace).size(), 1u);
  ASSERT_EQ(TestPackets(second_trace).size(), 1u);
  EXPECT_EQ(TestPackets(first_trace)[0].for_testing().str(), "both");
  EXPECT_EQ(TestPackets(second_trace)[0].for_testing().str(), "both");
}

// The service scrapes a no_flush data source instead of asking the producer
// to flush. The scrape cannot see the ring, so the v2 endpoint asks anyway:
// the tail must be in the service when the flush completes.
TEST_F(TracingV2InProcessTest,
       NoFlushDataSourceTailIsPresentWhenFlushCompletes) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfigFor({"tracing_v2_no_flush"}));
  session->StartBlocking();

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(1);
        packet->set_for_testing()->set_str("tail");
      });
  ASSERT_TRUE(session->FlushBlocking(/*timeout_ms=*/30000));

  // Read before stopping: stopping would flush again on its own.
  const std::vector<protos::gen::TracePacket> packets =
      ReadTestPackets(session.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "tail");
  session->StopBlocking();
}

TEST_F(TracingV2InProcessTest, FlushAndCloneSessionIncludesNoFlushTail) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  perfetto::TraceConfig cfg = MakeConfigFor({"tracing_v2_no_flush"});
  cfg.set_unique_session_name("tracing_v2_no_flush_clone");
  session->Setup(cfg);
  session->StartBlocking();

  TracingV2NoFlushDataSource::Trace(
      [](TracingV2NoFlushDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(2);
        packet->set_for_testing()->set_str("cloned tail");
      });

  // Cloning flushes the same set of instances a consumer flush would.
  std::unique_ptr<perfetto::TracingSession> clone =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
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

  const std::vector<protos::gen::TracePacket> packets =
      ReadTestPackets(clone.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), "cloned tail");
  session->StopBlocking();
}

// Flushing for the ring's sake must not invent an OnFlush(): one consumer
// flush, one callback for the flushing data source, none for the no_flush
// one, both tails in the service.
TEST_F(TracingV2InProcessTest, DeclaredNoFlushDataSourceGetsNoOnFlushCallback) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(
      MakeConfigFor({"tracing_v2_flushing", "tracing_v2_declared_no_flush"}));
  session->StartBlocking();

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
  EXPECT_EQ(strings, (std::vector<std::string>{"declared", "flushing"}));
  session->StopBlocking();
}

TEST_F(TracingV2InProcessTest, TeardownWithQueuedRelayWorkIsSafe) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfig());
  session->StartBlocking();
  for (uint32_t i = 0; i < 64; ++i) {
    TracingV2TestDataSource::Trace(
        [i](TracingV2TestDataSource::TraceContext ctx) {
          auto packet = ctx.NewTracePacket();
          packet->set_timestamp(i);
          packet->set_for_testing()->set_str("teardown");
        });
  }
  // Deliberately no flush and no quiescence wait: the session and the muxer are
  // torn down with packets still in the ring and a drain still queued.
  session->StopBlocking();
  session.reset();
  // The suite's TearDownTestSuite() runs ResetForTesting(), which is what tears
  // the bridge down with a drain still queued; reaching the end of this binary
  // without a crash or a hang is the assertion.
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
