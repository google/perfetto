#include "perfetto/tracing/tracing.h"

#include <stdio.h>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "perfetto/tracing/backend_type.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trigger.gen.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/tmp_dir_tree.h"
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

// Unregisters its data source from StartDataSource() instead of acking it.
class VanishingProducer : public Producer {
 public:
  static constexpr char kDataSourceName[] = "test_ds";

  // Calls `on_registered` (on the producer thread) once the service has
  // registered the data source.
  VanishingProducer(const std::string& producer_socket,
                    std::function<void()> on_registered)
      : runner_(base::ThreadTaskRunner::CreateAndStart("fake_app")),
        on_registered_(std::move(on_registered)) {
    runner_.PostTaskAndWaitForTesting([this, &producer_socket]() {
      endpoint_ = ProducerIPCClient::Connect(producer_socket.c_str(), this,
                                             "fake_app", &runner_);
    });
  }

  ~VanishingProducer() override {
    runner_.PostTaskAndWaitForTesting([this]() { endpoint_.reset(); });
  }

  bool start_reached() const { return start_reached_; }

  void OnConnect() override {
    DataSourceDescriptor descriptor;
    descriptor.set_name(kDataSourceName);
    descriptor.set_will_notify_on_start(true);
    endpoint_->RegisterDataSource(descriptor);
    // The service handles requests in order, so Sync() replies after the
    // registration completes.
    endpoint_->Sync(on_registered_);
  }

  void StartDataSource(DataSourceInstanceID, const DataSourceConfig&) override {
    start_reached_ = true;
    // Skip NotifyDataSourceStarted() so the data source vanishes before start.
    endpoint_->UnregisterDataSource(kDataSourceName);
  }

  void OnDisconnect() override {}
  void OnTracingSetup() override {}
  void SetupDataSource(DataSourceInstanceID, const DataSourceConfig&) override {
  }
  void StopDataSource(DataSourceInstanceID) override {}
  void Flush(FlushRequestID,
             const DataSourceInstanceID*,
             size_t,
             FlushFlags) override {}
  void ClearIncrementalState(const DataSourceInstanceID*, size_t) override {}

 private:
  base::ThreadTaskRunner runner_;
  std::function<void()> on_registered_;
  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;
  std::atomic<bool> start_reached_{false};
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

// Unregistering the only data source during start must still complete the
// session start.
TEST_F(TracingMuxerImplIntegrationTest,
       StartCompletesWhenDataSourceUnregistersDuringStart) {
  base::TmpDirTree tmpdir;
  base::TestTaskRunner task_runner;

  ASSERT_FALSE(perfetto::Tracing::IsInitialized());

  tmpdir.TrackFile("producer.sock");
  tmpdir.TrackFile("consumer.sock");
  TracingServiceThread tracing_service(tmpdir.AbsolutePath("producer.sock"),
                                       tmpdir.AbsolutePath("consumer.sock"));
  SetEnvVar("PERFETTO_PRODUCER_SOCK_NAME",
            tracing_service.producer_socket().c_str());
  SetEnvVar("PERFETTO_CONSUMER_SOCK_NAME",
            tracing_service.consumer_socket().c_str());

  // Wait for registration so Setup() creates an instance for this producer.
  auto registered = task_runner.CreateCheckpoint("registered");
  VanishingProducer producer(
      tracing_service.producer_socket(),
      [&task_runner, registered] { task_runner.PostTask(registered); });
  task_runner.RunUntilCheckpoint("registered");

  TracingInitArgs args;
  args.backends = perfetto::kSystemBackend;
  perfetto::Tracing::Initialize(args);

  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  cfg.add_data_sources()->mutable_config()->set_name(
      VanishingProducer::kDataSourceName);

  std::unique_ptr<TracingSession> session =
      perfetto::Tracing::NewTrace(perfetto::kSystemBackend);
  session->Setup(cfg);
  // The callback runs on the muxer thread. A failure times out here instead
  // of blocking forever, like StartBlocking() would.
  auto started = task_runner.CreateCheckpoint("started");
  session->SetOnStartCallback(
      [&task_runner, started] { task_runner.PostTask(started); });
  session->Start();
  task_runner.RunUntilCheckpoint("started", 30000);

  EXPECT_TRUE(producer.start_reached());

  session->StopBlocking();
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
