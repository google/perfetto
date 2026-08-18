#include "perfetto/tracing/tracing.h"

#include <stdio.h>
#include <optional>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "perfetto/tracing/backend_type.h"
#include "perfetto/tracing/data_source.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trigger.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/tracing/internal/tracing_muxer_impl.h"
#include "src/tracing/internal/tracing_v2_producer_endpoint.h"
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

// ---------------------------------------------------------------------------
// Tracing v2 in-process path.
//
// The v2 ring is meant to be invisible: an enabled session produces the same
// trace as a disabled one. So every test here both checks the trace and checks
// a narrow signal that the gate really took effect, because a test that only
// looked at the output would pass with the feature switched off.
// ---------------------------------------------------------------------------

class TracingV2TestDataSource
    : public perfetto::DataSource<TracingV2TestDataSource> {
 public:
  // So that a test can ask for a stalling writer through the config instead of
  // registering a second data source for it.
  constexpr static bool kBufferExhaustedPolicyConfigurable = true;

  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override {}
  void OnStop(const StopArgs&) override {}
};

// Tracing is initialized once for the whole suite rather than per test. That is
// not a style choice: repeatedly calling Tracing::Initialize() and
// ResetForTesting() from this fixture crashes in the fake system backend's
// producer connection, with or without the v2 gate, so a per-test cycle would
// be testing the harness rather than the ring.
class TracingV2InProcessTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (!perfetto::tracing_v2::kHasFutex)
      return;
    perfetto::internal::SetTracingV2InProcessForTesting(true);
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name("tracing_v2_test");
    TracingV2TestDataSource::Register(dsd);
    perfetto::test::SyncProducers();
    perfetto::test::DisableReconnectLimit();
  }

  static void TearDownTestSuite() {
    if (!perfetto::tracing_v2::kHasFutex)
      return;
    // Clear the gate first, so nothing later in this binary is silently run
    // with v2 enabled.
    perfetto::internal::SetTracingV2InProcessForTesting(false);
    // The data source's thread-local state points at muxer state that
    // ResetForTesting() is about to destroy.
    perfetto::test::TracingMuxerImplInternalsForTest::
        ClearDataSourceTlsStateOnReset<TracingV2TestDataSource>();
    perfetto::Tracing::ResetForTesting();
  }

  void SetUp() override {
    if (!perfetto::tracing_v2::kHasFutex)
      GTEST_SKIP() << "The tracing v2 ring needs a futex on this platform";
    // Every test in this suite runs with the gate on, so this also checks that
    // the decoration really happened rather than silently falling back to v1.
    ASSERT_TRUE(perfetto::internal::UseTracingV2InProcess());
  }

  static perfetto::TraceConfig MakeConfig() {
    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(1024);
    cfg.add_data_sources()->mutable_config()->set_name("tracing_v2_test");
    return cfg;
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

  // Everything published to a v2 ring before this returns has reached the
  // service. See the ordering limitation on TracingV2ProducerEndpoint: a
  // production control-plane call does not imply this, which is exactly why
  // the seam exists and why it is spelled ForTesting.
  static void WaitForRelay() {
    static_cast<perfetto::internal::TracingMuxerImpl*>(
        perfetto::internal::TracingMuxer::Get())
        ->WaitForTracingV2QuiescenceForTesting();
  }

  static protos::gen::Trace StopAndParse(perfetto::TracingSession* session) {
    WaitForRelay();
    session->StopBlocking();
    const std::vector<char> bytes = session->ReadTraceBlocking();
    // Lets a run save the trace it produced, so the v2 path can be checked
    // with the ordinary trace-processor tooling rather than only in-process.
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
  const uint64_t before =
      perfetto::internal::GetTracingV2WritersCreatedForTesting();
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
          // A nested message, so the private start-tag-and-terminator framing
          // is actually exercised and has to survive the rewrite.
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
  // The gate really took effect, which the trace above cannot show on its own.
  EXPECT_GT(perfetto::internal::GetTracingV2WritersCreatedForTesting(), before);
}

TEST_F(TracingV2InProcessTest, PacketLargerThanAChunkReconstructsExactly) {
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfig());
  session->StartBlocking();

  // Chunks are 256 bytes, so this packet is split across a dozen of them and
  // has to come back through the continuation flags.
  const std::string payload(4096, 'x');
  TracingV2TestDataSource::Trace(
      [&payload](TracingV2TestDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(7);
        packet->set_for_testing()->set_str(payload);
      });
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  const protos::gen::Trace trace = StopAndParse(session.get());
  const std::vector<protos::gen::TracePacket> packets = TestPackets(trace);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 7u);
  EXPECT_EQ(packets[0].for_testing().str(), payload);
}

// The previous test is larger than a chunk. This one is larger than the whole
// ring - 1024 x 256 bytes - which is a different property: the writer fills the
// ring more than twice over inside a single packet, so the relay has to run,
// and free capacity, while that packet is still being encoded. That works only
// because the writer says the ring moved at every fragment boundary rather than
// when the packet ends.
//
// A stalling writer, because a dropping one racing a relay on another thread
// would make this a test of which thread won. The exact ordering rule - close,
// notify, then ask for the next chunk - is pinned deterministically and in
// milliseconds by TraceWriterV2Test.PacketLargerThanTheWholeRingSurvives. What
// this adds is the real thread and task plumbing around it: the notification
// coalescing, the relay sequence, the continuation flags over a full ring
// traversal, and the rewrite back to a canonical packet.
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

  // No v2-layer loss anywhere in the trace. Only the reasons the relay reports
  // are checked: a packet this size is also larger than the downstream v1 hop's
  // own 256 KiB shared memory buffer and than a comfortable fraction of the
  // service's buffer, so v1 and service loss flags are a property of the
  // scaffolding rather than of the ring, and both disappear with this relay.
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

// The end-to-end evidence that leaves the process: a trace written through the
// v2 ring, saved and queried with the ordinary trace-processor tooling rather
// than only inspected in memory. ProcessTree is used because trace_processor
// turns it into rows a query can check, which a TestEvent packet would not.
// Run with PERFETTO_TRACING_V2_TRACE_OUT set to keep the file.
TEST_F(TracingV2InProcessTest, ProcessTreeSessionIsQueryable) {
  const uint64_t before =
      perfetto::internal::GetTracingV2WritersCreatedForTesting();
  std::unique_ptr<perfetto::TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(MakeConfig());
  session->StartBlocking();

  TracingV2TestDataSource::Trace([](TracingV2TestDataSource::TraceContext ctx) {
    auto packet = ctx.NewTracePacket();
    packet->set_timestamp(1);
    auto* tree = packet->set_process_tree();
    for (int32_t i = 0; i < 8; ++i) {
      auto* process = tree->add_processes();
      process->set_pid(4242 + i);
      process->set_ppid(1);
      process->add_cmdline("tracing_v2_e2e_" + std::to_string(i));
    }
  });
  TracingV2TestDataSource::Trace(
      [](TracingV2TestDataSource::TraceContext ctx) { ctx.Flush(); });

  const protos::gen::Trace trace = StopAndParse(session.get());
  uint32_t processes = 0;
  for (const protos::gen::TracePacket& packet : trace.packet()) {
    if (packet.has_process_tree())
      processes +=
          static_cast<uint32_t>(packet.process_tree().processes().size());
  }
  EXPECT_EQ(processes, 8u);
  EXPECT_GT(perfetto::internal::GetTracingV2WritersCreatedForTesting(), before);
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
