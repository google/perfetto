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

#include <chrono>
#include <cinttypes>
#include <thread>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/ipc/client.h"
#include "perfetto/ext/tracing/core/consumer.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/ipc/consumer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/service_ipc_host.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/ipc/test/test_socket.h"
#include "src/tracing/service/tracing_service_impl.h"
#include "src/tracing/v2/producer_ring.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "test/gtest_and_gmock.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <fcntl.h>
#include <signal.h>
#include "src/tracing/ipc/memfd.h"
#include "src/tracing/ipc/posix_shared_memory.h"
#endif

#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/ipc/producer_port.ipc.h"
#include "protos/perfetto/trace/clock_snapshot.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

using testing::_;
using testing::InvokeWithoutArgs;
using tracing_service::TracingServiceImpl;

ipc::TestSocket kProducerSock{"tracing_test-producer"};
ipc::TestSocket kConsumerSock{"tracing_test-consumer"};

// TODO(rsavitski): consider using src/tracing/test/mock_producer.h.
class MockProducer : public Producer {
 public:
  ~MockProducer() override {}

  // Producer implementation.
  MOCK_METHOD(void, OnConnect, (), (override));
  MOCK_METHOD(void, OnDisconnect, (), (override));
  MOCK_METHOD(void,
              SetupDataSource,
              (DataSourceInstanceID, const DataSourceConfig&),
              (override));
  MOCK_METHOD(void,
              StartDataSource,
              (DataSourceInstanceID, const DataSourceConfig&),
              (override));
  MOCK_METHOD(void, StopDataSource, (DataSourceInstanceID), (override));
  MOCK_METHOD(void, OnTracingSetup, (), (override));
  MOCK_METHOD(void,
              Flush,
              (FlushRequestID, const DataSourceInstanceID*, size_t, FlushFlags),
              (override));
  MOCK_METHOD(void,
              ClearIncrementalState,
              (const DataSourceInstanceID*, size_t),
              (override));
};

class MockConsumer : public Consumer {
 public:
  ~MockConsumer() override {}

  // Producer implementation.
  MOCK_METHOD(void, OnConnect, (), (override));
  MOCK_METHOD(void, OnDisconnect, (), (override));
  MOCK_METHOD(void,
              OnTracingDisabled,
              (const std::string& /*error*/),
              (override));
  MOCK_METHOD(void, OnTracePackets, (std::vector<TracePacket>*, bool));
  MOCK_METHOD(void, OnDetach, (bool), (override));
  MOCK_METHOD(void, OnAttach, (bool, const TraceConfig&), (override));
  MOCK_METHOD(void, OnTraceStats, (bool, const TraceStats&), (override));
  MOCK_METHOD(void, OnObservableEvents, (const ObservableEvents&), (override));
  MOCK_METHOD(void, OnSessionCloned, (const OnSessionClonedArgs&), (override));

  // Workaround, gmock doesn't support yet move-only types, passing a pointer.
  void OnTraceData(std::vector<TracePacket> packets, bool has_more) {
    OnTracePackets(&packets, has_more);
  }
};

void CheckTraceStats(const protos::gen::TracePacket& packet) {
  EXPECT_TRUE(packet.has_trace_stats());
  EXPECT_GE(packet.trace_stats().producers_seen(), 1u);
  EXPECT_EQ(1u, packet.trace_stats().producers_connected());
  EXPECT_EQ(1u, packet.trace_stats().data_sources_registered());
  EXPECT_EQ(1u, packet.trace_stats().tracing_sessions());
  EXPECT_EQ(1u, packet.trace_stats().total_buffers());
  EXPECT_EQ(1, packet.trace_stats().buffer_stats_size());

  const auto& buf_stats = packet.trace_stats().buffer_stats()[0];
  EXPECT_GT(buf_stats.bytes_written(), 0u);
  EXPECT_GT(buf_stats.chunks_written(), 0u);
  EXPECT_EQ(0u, buf_stats.chunks_overwritten());
  EXPECT_EQ(0u, buf_stats.chunks_rewritten());
  EXPECT_EQ(0u, buf_stats.chunks_committed_out_of_order());
  EXPECT_EQ(0u, buf_stats.write_wrap_count());
  EXPECT_EQ(0u, buf_stats.patches_failed());
  EXPECT_EQ(0u, buf_stats.readaheads_failed());
  EXPECT_EQ(0u, buf_stats.abi_violations());
}

static_assert(TracingServiceImpl::kMaxTracePacketSliceSize <=
                  ipc::kIPCBufferSize - 512,
              "Tracing service max packet slice should be smaller than IPC "
              "buffer size (with some headroom)");

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Forwards ProducerRing notifications through the real IPC endpoint.
// The muxer owns this adapter in production.
class EndpointServiceChannel : public tracing_v2::ProducerRing::ServiceChannel {
 public:
  explicit EndpointServiceChannel(TracingService::ProducerEndpoint* endpoint)
      : endpoint_(endpoint) {}
  void NotifyRingData(std::function<void()> on_picked_up) override {
    endpoint_->NotifyTracingV2RingData(
        [cb = std::move(on_picked_up)](bool success) {
          if (success && cb)
            cb();
        });
  }
  void RetireWriter(WriterID writer_id, std::function<void(bool)> cb) override {
    endpoint_->RetireTracingV2Writer(writer_id, std::move(cb));
  }
  bool DrainRunsOnCurrentThread() override {
    return endpoint_->IsTracingV2DrainOnCurrentThread();
  }

 private:
  TracingService::ProducerEndpoint* const endpoint_;
};
#endif  // !PERFETTO_OS_WIN

}  // namespace

class TracingIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
    kProducerSock.Destroy();
    kConsumerSock.Destroy();
    task_runner_.reset(new base::TestTaskRunner());

    StartService();

    // Create and connect a Producer.
    producer_endpoint_ = ProducerIPCClient::Connect(
        kProducerSock.name(), &producer_, "perfetto.mock_producer",
        task_runner_.get(), GetProducerSMBScrapingMode());
    auto on_producer_connect =
        task_runner_->CreateCheckpoint("on_producer_connect");
    EXPECT_CALL(producer_, OnConnect()).WillOnce(on_producer_connect);
    task_runner_->RunUntilCheckpoint("on_producer_connect");

    // Register a data source.
    DataSourceDescriptor ds_desc;
    ds_desc.set_name("perfetto.test");
    producer_endpoint_->RegisterDataSource(ds_desc);

    // Create and connect a Consumer.
    consumer_endpoint_ = ConsumerIPCClient::Connect(
        kConsumerSock.name(), &consumer_, task_runner_.get());
    auto on_consumer_connect =
        task_runner_->CreateCheckpoint("on_consumer_connect");
    EXPECT_CALL(consumer_, OnConnect()).WillOnce(on_consumer_connect);
    task_runner_->RunUntilCheckpoint("on_consumer_connect");

    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&producer_));
    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&consumer_));
  }

  void TearDown() override {
    // Destroy the service and check that both Producer and Consumer see an
    // OnDisconnect() call.

    auto on_producer_disconnect =
        task_runner_->CreateCheckpoint("on_producer_disconnect");
    EXPECT_CALL(producer_, OnDisconnect()).WillOnce(on_producer_disconnect);

    auto on_consumer_disconnect =
        task_runner_->CreateCheckpoint("on_consumer_disconnect");
    EXPECT_CALL(consumer_, OnDisconnect()).WillOnce(on_consumer_disconnect);

    svc_.reset();
    if (service_process_.status() == base::Subprocess::kRunning)
      service_process_.KillAndWaitForTermination();
    task_runner_->RunUntilCheckpoint("on_producer_disconnect");
    task_runner_->RunUntilCheckpoint("on_consumer_disconnect");

    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&producer_));
    ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&consumer_));

    task_runner_.reset();
    kProducerSock.Destroy();
    kConsumerSock.Destroy();
  }

  virtual TracingService::ProducerSMBScrapingMode GetProducerSMBScrapingMode() {
    return TracingService::ProducerSMBScrapingMode::kDefault;
  }

  virtual void StartService() {
    svc_ = ServiceIPCHost::CreateInstance(task_runner_.get());
    svc_->Start(kProducerSock.name(), kConsumerSock.name());
  }

  base::Subprocess service_process_;

  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<ServiceIPCHost> svc_;
  std::unique_ptr<TracingService::ProducerEndpoint> producer_endpoint_;
  MockProducer producer_;
  std::unique_ptr<TracingService::ConsumerEndpoint> consumer_endpoint_;
  MockConsumer consumer_;
};

class TracingIntegrationTestWithChunkSize
    : public TracingIntegrationTest,
      public testing::WithParamInterface<uint32_t> {};

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
namespace {
class RingAdoptionListener : public ipc::ServiceProxy::EventListener {
 public:
  std::function<void()> connected;
  void OnConnect() override { connected(); }
};

// Exercises the FD and geometry trust boundary through the real IPC service.
void ExpectRingAdoption(base::TestTaskRunner* runner,
                        int fd,
                        uint32_t chunks,
                        uint32_t chunk_size,
                        bool should_accept,
                        bool check_notification = false,
                        bool supports_v2 = true) {
  RingAdoptionListener listener;
  listener.connected = runner->CreateCheckpoint("ring_bound");
  auto client =
      ipc::Client::CreateInstance({kProducerSock.name(), false}, runner);
  protos::gen::ProducerPortProxy proxy(&listener);
  client->BindService(proxy.GetWeakPtr());
  runner->RunUntilCheckpoint("ring_bound");
  auto initialized = runner->CreateCheckpoint("ring_initialized");
  ipc::Deferred<protos::gen::InitializeConnectionResponse> init_response;
  init_response.Bind([&](auto response) {
    EXPECT_TRUE(response.success());
    if (response.success())
      EXPECT_EQ(response->tracing_v2_direct_transport_supported(), supports_v2);
    initialized();
  });
  protos::gen::InitializeConnectionRequest init;
  init.set_producer_name("ring-producer");
  if (supports_v2)
    init.set_tracing_v2_direct_transport_supported(true);
  proxy.InitializeConnection(init, std::move(init_response));
  runner->RunUntilCheckpoint("ring_initialized");

  auto adopted = runner->CreateCheckpoint("ring_adopted");
  ipc::Deferred<protos::gen::AdoptTracingV2RingResponse> response;
  response.Bind([&](auto result) {
    EXPECT_EQ(result.success(), should_accept);
    adopted();
  });
  protos::gen::AdoptTracingV2RingRequest req;
  req.set_num_chunks(chunks);
  req.set_chunk_size_bytes(chunk_size);
  proxy.AdoptTracingV2Ring(req, std::move(response), fd);
  runner->RunUntilCheckpoint("ring_adopted");

  if (check_notification) {
    auto notified = runner->CreateCheckpoint("ring_notified");
    ipc::Deferred<protos::gen::NotifyTracingV2RingDataResponse> notify_response;
    notify_response.Bind([&](auto result) {
      EXPECT_FALSE(result.success())
          << "A protocol error must not ACK a successful drain";
      notified();
    });
    proxy.NotifyTracingV2RingData({}, std::move(notify_response));
    runner->RunUntilCheckpoint("ring_notified");
  }
}
}  // namespace

TEST_F(TracingIntegrationTest, TracingV2EmptySealedRingRejectedNonfatally) {
  auto fd = CreateMemfd("ring-empty", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  ASSERT_TRUE(fd);
  ASSERT_EQ(fcntl(*fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL),
            0);
  ExpectRingAdoption(task_runner_.get(), *fd, 2, 256, false);
}

TEST_F(TracingIntegrationTest, TracingV2RequiresProducerCapability) {
  auto memory = PosixSharedMemory::Create(tracing_v2::RingLogicalSize(2, 256));
  ASSERT_TRUE(memory);
  ExpectRingAdoption(task_runner_.get(), memory->fd(), 2, 256, false,
                     /*check_notification=*/false, /*supports_v2=*/false);
}

TEST_F(TracingIntegrationTest, TracingV2ReadOnlyRingRejectedNonfatally) {
  auto memory = PosixSharedMemory::Create(tracing_v2::RingLogicalSize(2, 256));
  ASSERT_TRUE(memory);
  std::string path = "/proc/self/fd/" + std::to_string(memory->fd());
  base::ScopedFile readonly(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  ASSERT_TRUE(readonly);
  ExpectRingAdoption(task_runner_.get(), *readonly, 2, 256, false);
}

TEST_F(TracingIntegrationTest, TracingV2InvalidGeometryReturnsFailure) {
  auto memory = PosixSharedMemory::Create(64 * 1024);
  ASSERT_TRUE(memory);
  ExpectRingAdoption(task_runner_.get(), memory->fd(), 3, 4096, false);
}

TEST_F(TracingIntegrationTest, TracingV2WriteSealedRingRejectedNonfatally) {
  auto fd = CreateMemfd("ring-write-sealed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  ASSERT_TRUE(fd);
  ASSERT_EQ(ftruncate(*fd, 4096), 0);
  ASSERT_EQ(fcntl(*fd, F_ADD_SEALS,
                  F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL),
            0);
  ExpectRingAdoption(task_runner_.get(), *fd, 2, 256, false);
}

TEST_F(TracingIntegrationTest, TracingV2AdoptionCallbackCanDeleteClient) {
  testing::NiceMock<MockProducer> producer;
  auto connected = task_runner_->CreateCheckpoint("extra_connected");
  EXPECT_CALL(producer, OnConnect()).WillOnce(connected);
  auto endpoint = ProducerIPCClient::Connect(kProducerSock.name(), &producer,
                                             "extra", task_runner_.get());
  task_runner_->RunUntilCheckpoint("extra_connected");
  auto done = task_runner_->CreateCheckpoint("rejected");
  TracingService::ProducerEndpoint::AdoptTracingV2RingArgs args;
  args.shared_memory = endpoint->CreateTracingV2Ring(4096);
  args.num_chunks = 3;
  args.chunk_size = 256;
  endpoint->AdoptTracingV2Ring(std::move(args), [&](bool success) {
    EXPECT_FALSE(success);
    endpoint.reset();
    done();
  });
  task_runner_->RunUntilCheckpoint("rejected");
}

TEST_F(TracingIntegrationTest, TracingV2ProtocolErrorDoesNotAckDrain) {
  auto memory = PosixSharedMemory::Create(tracing_v2::RingLogicalSize(2, 256));
  ASSERT_TRUE(memory);
  auto* header = static_cast<tracing_v2::RingBufferHeader*>(memory->start());
  header->rw_positions.store(tracing_v2::PackRwPositions(3, 0));
  ExpectRingAdoption(task_runner_.get(), memory->fd(), 2, 256, true, true);
}

class TracingV2ProcessTest : public TracingIntegrationTest {
 public:
  void StartService() override {
    // Publish before either process opens a service connection. Adoption binds
    // target 42 after setup and must preserve these bytes and positions.
    early_memory_ =
        PosixSharedMemory::Create(tracing_v2::RingLogicalSize(4, 4096));
    early_ring_ =
        tracing_v2::ProducerRing::Create(early_memory_, 4, 4096, nullptr);
    auto writer =
        early_ring_->CreateTraceWriter(42, BufferExhaustedPolicy::kDrop);
    writer->NewTracePacket()->set_for_testing()->set_str("before_handshake");
    writer.reset();
    auto ready = base::Pipe::Create();
    const int ready_fd = *ready.wr;
    service_process_.args.exec_cmd = {base::GetCurExecutableDir() + "/traced"};
    service_process_.args.env = {
        "PERFETTO_PRODUCER_SOCK_NAME=" + std::string(kProducerSock.name()),
        "PERFETTO_CONSUMER_SOCK_NAME=" + std::string(kConsumerSock.name()),
        "TRACED_NOTIFY_FD=" + std::to_string(ready_fd)};
    if (const char* libraries = getenv("LD_LIBRARY_PATH"))
      service_process_.args.env.push_back(std::string("LD_LIBRARY_PATH=") +
                                          libraries);
    service_process_.args.preserve_fds = {ready_fd};
    service_process_.Start();
    ready.wr.reset();
    auto started = task_runner_->CreateCheckpoint("service_ready");
    task_runner_->AddFileDescriptorWatch(*ready.rd, [&] {
      char byte = 0;
      ASSERT_EQ(read(*ready.rd, &byte, 1), 1);
      ASSERT_EQ(byte, '1');
      task_runner_->RemoveFileDescriptorWatch(*ready.rd);
      started();
    });
    task_runner_->RunUntilCheckpoint("service_ready");
  }
  std::shared_ptr<SharedMemory> early_memory_;
  std::shared_ptr<tracing_v2::ProducerRing> early_ring_;
};

TEST_F(TracingV2ProcessTest, FullRingWakesProducerAcrossProcesses) {
  TraceConfig config;
  auto* buffer = config.add_buffers();
  buffer->set_size_kb(4096);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* ds = config.add_data_sources()->mutable_config();
  ds->set_name("perfetto.test");
  ds->set_use_tracing_v2(true);
  config.add_producers()->set_shm_size_kb(32);
  config.mutable_producers()->back().set_producer_name(
      "perfetto.mock_producer");
  BufferID target = 0;
  auto started = task_runner_->CreateCheckpoint("started");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([&](DataSourceInstanceID, const DataSourceConfig& cfg) {
        target = static_cast<BufferID>(cfg.target_buffer());
        started();
      });
  consumer_endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("started");
  ASSERT_EQ(producer_endpoint_->shared_memory(), nullptr);
  ASSERT_EQ(producer_endpoint_->MaybeSharedMemoryArbiter(), nullptr);
  ASSERT_EQ(producer_endpoint_->tracing_v2_ring_size_bytes(), 32u * 1024);

  constexpr uint32_t kChunks = 4;
  constexpr uint32_t kChunkSize = 4096;
  auto memory = early_memory_;
  EndpointServiceChannel channel(producer_endpoint_.get());
  auto ring = early_ring_;
  ASSERT_TRUE(ring);
  auto adopted = task_runner_->CreateCheckpoint("adopted");
  producer_endpoint_->AdoptTracingV2Ring(
      {memory, kChunks, kChunkSize, {{42, target}}}, [&](bool success) {
        ASSERT_TRUE(success);
        adopted();
      });
  task_runner_->RunUntilCheckpoint("adopted");
  ASSERT_TRUE(ring->AttachToService(&channel));

  // Stop only this test's service. The writer must park on the shared futex
  // before the service resumes, so the test proves an actual cross-process
  // wake.
  ASSERT_EQ(kill(service_process_.pid(), SIGSTOP), 0);
  const std::string payload(300 * 1024, 'p');
  auto flushed = task_runner_->CreateCheckpoint("flushed");
  std::thread writer_thread([&] {
    auto writer = ring->CreateTraceWriter(42, BufferExhaustedPolicy::kStall);
    writer->NewTracePacket()->set_for_testing()->set_str(payload);
    writer->Flush(flushed);
  });
  auto* header = static_cast<tracing_v2::RingBufferHeader*>(memory->start());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!header->num_writers_waiting.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_GT(header->num_writers_waiting.load(), 0u);
  EXPECT_EQ(kill(service_process_.pid(), SIGCONT), 0);
  task_runner_->RunUntilCheckpoint("flushed");
  writer_thread.join();

  auto read_done = task_runner_->CreateCheckpoint("read_done");
  size_t received = 0;
  size_t early_received = 0;
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&](std::vector<TracePacket>* packets, bool more) {
        for (auto& packet : *packets) {
          protos::gen::TracePacket decoded;
          ASSERT_TRUE(decoded.ParseFromString(packet.GetRawBytesForTesting()));
          if (decoded.has_for_testing()) {
            if (decoded.for_testing().str() == "before_handshake") {
              EXPECT_EQ(decoded.previous_packet_dropped(), 0u);
              ++early_received;
              continue;
            }
            EXPECT_EQ(decoded.for_testing().str(), payload);
            ++received;
          }
        }
        if (!more)
          read_done();
      });
  consumer_endpoint_->ReadBuffers();
  task_runner_->RunUntilCheckpoint("read_done");
  EXPECT_EQ(received, 1u);
  EXPECT_EQ(early_received, 1u);

  // This thread sends drain RPCs. Both stall policies must drop when this
  // thread fills the ring, since parking here prevents those RPCs from running.
  size_t recovery_count = 0;
  for (auto policy :
       {BufferExhaustedPolicy::kStall, BufferExhaustedPolicy::kStallThenDrop}) {
    auto writer = ring->CreateTraceWriter(42, policy);
    writer->NewTracePacket()->set_for_testing()->set_str(payload);
    auto drained = task_runner_->CreateCheckpoint(
        "drain_pressure_" + std::to_string(recovery_count));
    writer->Flush(drained);
    task_runner_->RunUntilCheckpoint("drain_pressure_" +
                                     std::to_string(recovery_count));
    writer->NewTracePacket()->set_for_testing()->set_str("loss_marker");
    auto marker = task_runner_->CreateCheckpoint(
        "drain_marker_" + std::to_string(recovery_count));
    writer->Flush(marker);
    task_runner_->RunUntilCheckpoint("drain_marker_" +
                                     std::to_string(recovery_count));
    writer->NewTracePacket()->set_for_testing()->set_str("recovered");
    auto recovered = task_runner_->CreateCheckpoint(
        "recovered_" + std::to_string(recovery_count));
    writer->Flush(recovered);
    task_runner_->RunUntilCheckpoint("recovered_" +
                                     std::to_string(recovery_count));
    ++recovery_count;
  }
  auto recovered_data = task_runner_->CreateCheckpoint("recovered_data");
  recovery_count = 0;
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&](std::vector<TracePacket>* packets, bool more) {
        for (auto& packet : *packets) {
          protos::gen::TracePacket decoded;
          ASSERT_TRUE(decoded.ParseFromString(packet.GetRawBytesForTesting()));
          if (decoded.has_for_testing()) {
            EXPECT_EQ(decoded.for_testing().str(), "recovered");
            EXPECT_NE(decoded.previous_packet_dropped(), 0u);
            ++recovery_count;
          }
        }
        if (!more)
          recovered_data();
      });
  consumer_endpoint_->ReadBuffers();
  task_runner_->RunUntilCheckpoint("recovered_data");
  EXPECT_EQ(recovery_count, 2u);
  ring->DetachFromService();
  auto stopped = task_runner_->CreateCheckpoint("stopped");
  EXPECT_CALL(producer_, StopDataSource(_))
      .WillOnce([&](DataSourceInstanceID id) {
        producer_endpoint_->NotifyDataSourceStopped(id);
      });
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(stopped));
  consumer_endpoint_->DisableTracing();
  task_runner_->RunUntilCheckpoint("stopped");
}
#endif

TEST_P(TracingIntegrationTestWithChunkSize, WithIPCTransport) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  if (GetParam() != 0) {
    auto* producer_config = trace_config.add_producers();
    producer_config->set_producer_name("perfetto.mock_producer");
    producer_config->set_tracing_v2_chunk_size_bytes(GetParam());
  }
  EXPECT_EQ(producer_endpoint_->tracing_v2_chunk_size_bytes(), 0u);
  consumer_endpoint_->EnableTracing(trace_config);

  // At this point, the Producer should be asked to turn its data source on.
  DataSourceInstanceID ds_iid = 0;

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup()).WillOnce([this] {
    EXPECT_EQ(producer_endpoint_->tracing_v2_chunk_size_bytes(), GetParam());
  });

  // Store the arguments passed to SetupDataSource() and later check that they
  // match the ones passed to StartDataSource().
  DataSourceInstanceID setup_id;
  DataSourceConfig setup_cfg_proto;
  EXPECT_CALL(producer_, SetupDataSource(_, _))
      .WillOnce([&setup_id, &setup_cfg_proto](DataSourceInstanceID id,
                                              const DataSourceConfig& cfg) {
        setup_id = id;
        setup_cfg_proto = cfg;
      });
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &ds_iid, &global_buf_id, &setup_id,
                 &setup_cfg_proto](DataSourceInstanceID id,
                                   const DataSourceConfig& cfg) {
        // id and config should match the ones passed to SetupDataSource.
        ASSERT_EQ(id, setup_id);
        ASSERT_EQ(setup_cfg_proto, cfg);
        ASSERT_NE(0u, id);
        ds_iid = id;
        ASSERT_EQ("perfetto.test", cfg.name());
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        ASSERT_NE(0u, global_buf_id);
        ASSERT_LE(global_buf_id, std::numeric_limits<BufferID>::max());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  // Now let the data source fill some pages within the same task.
  // Doing so should accumulate a bunch of chunks that will be notified by the
  // a future task in one batch.
  std::unique_ptr<TraceWriter> writer = producer_endpoint_->CreateTraceWriter(
      global_buf_id, BufferExhaustedPolicy::kStall);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[16];
    base::SprintfTrunc(buf, sizeof(buf), "evt_%zu", i);
    writer->NewTracePacket()->set_for_testing()->set_str(buf, strlen(buf));
  }

  // Allow the service to see the CommitData() before reading back.
  auto on_data_committed = task_runner_->CreateCheckpoint("on_data_committed");
  writer->Flush(on_data_committed);
  task_runner_->RunUntilCheckpoint("on_data_committed");

  // Read the log buffer.
  consumer_endpoint_->ReadBuffers();
  size_t num_pack_rx = 0;
  bool saw_clock_snapshot = false;
  bool saw_trace_config = false;
  bool saw_trace_stats = false;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&num_pack_rx, all_packets_rx, &trace_config,
                       &saw_clock_snapshot, &saw_trace_config,
                       &saw_trace_stats](std::vector<TracePacket>* packets,
                                         bool has_more) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
        const int kExpectedMinNumberOfClocks = 1;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
        const int kExpectedMinNumberOfClocks = 2;
#else
        const int kExpectedMinNumberOfClocks = 6;
#endif

        for (auto& encoded_packet : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(
              packet.ParseFromString(encoded_packet.GetRawBytesForTesting()));
          if (packet.has_for_testing()) {
            char buf[8];
            base::SprintfTrunc(buf, sizeof(buf), "evt_%zu", num_pack_rx++);
            EXPECT_EQ(std::string(buf), packet.for_testing().str());
          } else if (packet.has_clock_snapshot()) {
            EXPECT_GE(packet.clock_snapshot().clocks_size(),
                      kExpectedMinNumberOfClocks);
            saw_clock_snapshot = true;
          } else if (packet.has_trace_config()) {
            EXPECT_EQ(packet.trace_config(), trace_config);
            saw_trace_config = true;
          } else if (packet.has_trace_stats()) {
            saw_trace_stats = true;
            CheckTraceStats(packet);
          }
        }
        if (!has_more)
          all_packets_rx();
      });
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  ASSERT_EQ(kNumPackets, num_pack_rx);
  EXPECT_TRUE(saw_clock_snapshot);
  EXPECT_TRUE(saw_trace_config);
  EXPECT_TRUE(saw_trace_stats);

  // Disable tracing.
  consumer_endpoint_->DisableTracing();

  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(producer_, StopDataSource(_));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
}

// Zero leaves the field absent. Valid explicit values reach the producer even
// if it only creates v1 writers. The 260-byte case verifies that chunk sizes
// need not be powers of two.
INSTANTIATE_TEST_SUITE_P(ChunkSize,
                         TracingIntegrationTestWithChunkSize,
                         testing::Values(0u, 1024u, 260u));

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Exercises direct v2 transport through a real IPC socket.
// The producer allocates a sealed memfd ring and passes its FD for adoption.
// One packet exceeds 128 KiB and spans many chunks. The service must read the
// ring directly and deliver all packets to the consumer.
TEST_F(TracingIntegrationTest, TracingV2DirectRingOverIPC) {
  // The service advertised direct v2 transport support at connection setup.
  ASSERT_TRUE(producer_endpoint_->IsTracingV2DirectTransportSupported());

  TraceConfig trace_config;
  auto* buffer = trace_config.add_buffers();
  buffer->set_size_kb(4096);
  buffer->set_experimental_mode(TraceConfig::BufferConfig::TRACE_BUFFER_V2);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  ds_config->set_use_tracing_v2(true);
  auto* producer_config = trace_config.add_producers();
  producer_config->set_producer_name("perfetto.mock_producer");
  producer_config->set_shm_size_kb(1024);
  consumer_endpoint_->EnableTracing(trace_config);

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &global_buf_id](
                    DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");
  ASSERT_NE(global_buf_id, 0u);

  // The producer allocates its own ring in a sealed memfd and hands it to the
  // service over IPC. 128 chunks of 4 KiB (512 KiB) comfortably hold the large
  // packet below without the single-threaded writer having to stall.
  constexpr uint32_t kNumChunks = 128;
  constexpr uint32_t kChunkSize = 4096;
  auto shmem = std::shared_ptr<SharedMemory>(
      PosixSharedMemory::Create(static_cast<size_t>(
          tracing_v2::RingLogicalSize(kNumChunks, kChunkSize))));
  ASSERT_TRUE(shmem);
  EndpointServiceChannel channel(producer_endpoint_.get());
  auto ring =
      tracing_v2::ProducerRing::Create(shmem, kNumChunks, kChunkSize, &channel);
  ASSERT_TRUE(ring);
  TracingService::ProducerEndpoint::AdoptTracingV2RingArgs adopt_args;
  adopt_args.shared_memory = shmem;
  adopt_args.num_chunks = kNumChunks;
  adopt_args.chunk_size = kChunkSize;
  bool ring_adopted = false;
  producer_endpoint_->AdoptTracingV2Ring(
      std::move(adopt_args),
      [&ring_adopted](bool accepted) { ring_adopted = accepted; });

  std::unique_ptr<TraceWriter> writer =
      ring->CreateTraceWriter(global_buf_id, BufferExhaustedPolicy::kDrop);
  ASSERT_TRUE(writer);
  writer->NewTracePacket()->set_for_testing()->set_str("small_before");
  const std::string big(160 * 1024,
                        'x');  // > 128 KiB: fragments across chunks.
  writer->NewTracePacket()->set_for_testing()->set_str(big);
  writer->NewTracePacket()->set_for_testing()->set_str("small_after");

  // Flush requests a drain through NotifyTracingV2RingData().
  // The service drains the ring, then replies. That reply invokes the flush
  // callback.
  auto on_flush = task_runner_->CreateCheckpoint("on_flush");
  writer->Flush(on_flush);
  task_runner_->RunUntilCheckpoint("on_flush");

  // The adoption RPC resolved before the flush drained the ring.
  EXPECT_TRUE(ring_adopted);

  consumer_endpoint_->ReadBuffers();
  bool saw_small_before = false, saw_big = false, saw_small_after = false;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&](std::vector<TracePacket>* packets, bool has_more) {
        for (auto& encoded_packet : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(
              packet.ParseFromString(encoded_packet.GetRawBytesForTesting()));
          if (!packet.has_for_testing())
            continue;
          const std::string& s = packet.for_testing().str();
          if (s == "small_before")
            saw_small_before = true;
          else if (s == "small_after")
            saw_small_after = true;
          else if (s == big)
            saw_big = true;
        }
        if (!has_more)
          all_packets_rx();
      });
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  EXPECT_TRUE(saw_small_before);
  EXPECT_TRUE(saw_big);
  EXPECT_TRUE(saw_small_after);

  consumer_endpoint_->DisableTracing();
  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(producer_, StopDataSource(_))
      .WillOnce([&](DataSourceInstanceID id) {
        producer_endpoint_->NotifyDataSourceStopped(id);
      });
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
  ring->DetachFromService();
}
#endif  // !PERFETTO_OS_WIN

TEST_F(TracingIntegrationTest, InvalidTracingV2ChunkSizeRejectsConfig) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.add_data_sources()->mutable_config()->set_name("perfetto.test");
  auto* producer_config = trace_config.add_producers();
  producer_config->set_producer_name("perfetto.mock_producer");
  producer_config->set_tracing_v2_chunk_size_bytes(255);

  // Invalid producer settings reject the config even for v1 data sources.
  // Wait for that rejection, rather than for a data source that cannot start.
  EXPECT_CALL(producer_, OnTracingSetup()).Times(0);
  EXPECT_CALL(producer_, SetupDataSource(_, _)).Times(0);
  EXPECT_CALL(producer_, StartDataSource(_, _)).Times(0);
  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(consumer_, OnTracingDisabled(
                             testing::HasSubstr("tracing_v2_chunk_size_bytes")))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  consumer_endpoint_->EnableTracing(trace_config);
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
  EXPECT_EQ(producer_endpoint_->shared_memory(), nullptr);
}

// Regression test for b/172950370.
TEST_F(TracingIntegrationTest, ValidErrorOnDisconnection) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  consumer_endpoint_->EnableTracing(trace_config);

  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());

  // Store the arguments passed to SetupDataSource() and later check that they
  // match the ones passed to StartDataSource().
  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce(InvokeWithoutArgs(on_create_ds_instance));
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce([](const std::string& err) {
        EXPECT_THAT(err,
                    testing::HasSubstr("EnableTracing IPC request rejected"));
      });

  // TearDown() will destroy the service via svc_.reset(). That will drop the
  // connection and trigger the EXPECT_CALL(OnTracingDisabled) above.
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST_F(TracingIntegrationTest, WriteIntoFile) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  trace_config.set_write_into_file(true);
  base::TempFile tmp_file = base::TempFile::CreateUnlinked();
  consumer_endpoint_->EnableTracing(trace_config,
                                    base::ScopedFile(dup(tmp_file.fd())));

  // At this point, the producer_ should be asked to turn its data source on.
  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());
  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &global_buf_id](
                    DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  std::unique_ptr<TraceWriter> writer = producer_endpoint_->CreateTraceWriter(
      global_buf_id, BufferExhaustedPolicy::kStall);
  ASSERT_TRUE(writer);

  const size_t kNumPackets = 10;
  for (size_t i = 0; i < kNumPackets; i++) {
    char buf[16];
    base::SprintfTrunc(buf, sizeof(buf), "evt_%zu", i);
    writer->NewTracePacket()->set_for_testing()->set_str(buf, strlen(buf));
  }
  auto on_data_committed = task_runner_->CreateCheckpoint("on_data_committed");
  writer->Flush(on_data_committed);
  task_runner_->RunUntilCheckpoint("on_data_committed");

  // Will disable tracing and will force the buffers to be written into the
  // file before destroying them.
  consumer_endpoint_->FreeBuffers();

  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  EXPECT_CALL(producer_, StopDataSource(_));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");

  // Check that |tmp_file| contains a valid trace.proto message.
  ASSERT_EQ(0, lseek(tmp_file.fd(), 0, SEEK_SET));
  std::string trace_contents;
  ASSERT_TRUE(base::ReadFileDescriptor(tmp_file.fd(), &trace_contents));
  protos::gen::Trace tmp_trace;
  ASSERT_TRUE(tmp_trace.ParseFromString(trace_contents));
  size_t num_test_packet = 0;
  size_t num_clock_snapshot_packet = 0;
  size_t num_system_info_packet = 0;
  bool saw_trace_stats = false;
  for (int i = 0; i < tmp_trace.packet_size(); i++) {
    const auto& packet = tmp_trace.packet()[static_cast<size_t>(i)];
    if (packet.has_for_testing()) {
      ASSERT_EQ("evt_" + std::to_string(num_test_packet++),
                packet.for_testing().str());
    } else if (packet.has_trace_stats()) {
      saw_trace_stats = true;
      CheckTraceStats(packet);
    } else if (packet.has_clock_snapshot()) {
      num_clock_snapshot_packet++;
    } else if (packet.has_system_info()) {
      num_system_info_packet++;
    }
  }
  ASSERT_TRUE(saw_trace_stats);
  ASSERT_GT(num_clock_snapshot_packet, 0u);
  ASSERT_GT(num_system_info_packet, 0u);
}
#endif

class TracingIntegrationTestWithSMBScrapingProducer
    : public TracingIntegrationTest {
 public:
  TracingService::ProducerSMBScrapingMode GetProducerSMBScrapingMode()
      override {
    return TracingService::ProducerSMBScrapingMode::kEnabled;
  }
};

TEST_F(TracingIntegrationTestWithSMBScrapingProducer, ScrapeOnFlush) {
  // Start tracing.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  consumer_endpoint_->EnableTracing(trace_config);

  // At this point, the Producer should be asked to turn its data source on.

  BufferID global_buf_id = 0;
  auto on_create_ds_instance =
      task_runner_->CreateCheckpoint("on_create_ds_instance");
  EXPECT_CALL(producer_, OnTracingSetup());

  EXPECT_CALL(producer_, SetupDataSource(_, _));
  EXPECT_CALL(producer_, StartDataSource(_, _))
      .WillOnce([on_create_ds_instance, &global_buf_id](
                    DataSourceInstanceID, const DataSourceConfig& cfg) {
        global_buf_id = static_cast<BufferID>(cfg.target_buffer());
        on_create_ds_instance();
      });
  task_runner_->RunUntilCheckpoint("on_create_ds_instance");

  // Create writer, which will post a task to register the writer with the
  // service.
  std::unique_ptr<TraceWriter> writer = producer_endpoint_->CreateTraceWriter(
      global_buf_id, BufferExhaustedPolicy::kStall);
  ASSERT_TRUE(writer);

  // Wait for the writer to be registered.
  task_runner_->RunUntilIdle();

  // Write a few trace packets.
  writer->NewTracePacket()->set_for_testing()->set_str("payload1");
  writer->NewTracePacket()->set_for_testing()->set_str("payload2");
  writer->NewTracePacket()->set_for_testing()->set_str("payload3");

  // Ask the service to flush, but don't flush our trace writer. This should
  // cause our uncommitted SMB chunk to be scraped.
  auto on_flush_complete = task_runner_->CreateCheckpoint("on_flush_complete");
  FlushFlags flush_flags(FlushFlags::Initiator::kConsumerSdk,
                         FlushFlags::Reason::kExplicit);
  consumer_endpoint_->Flush(
      5000,
      [on_flush_complete](bool success) {
        EXPECT_TRUE(success);
        on_flush_complete();
      },
      flush_flags);
  EXPECT_CALL(producer_, Flush(_, _, _, flush_flags))
      .WillOnce([this](FlushRequestID flush_req_id, const DataSourceInstanceID*,
                       size_t, FlushFlags) {
        producer_endpoint_->NotifyFlushComplete(flush_req_id);
      });
  task_runner_->RunUntilCheckpoint("on_flush_complete");

  // Read the log buffer. We should see all the packets.
  consumer_endpoint_->ReadBuffers();

  size_t num_test_pack_rx = 0;
  auto all_packets_rx = task_runner_->CreateCheckpoint("all_packets_rx");
  EXPECT_CALL(consumer_, OnTracePackets(_, _))
      .WillRepeatedly([&num_test_pack_rx, all_packets_rx](
                          std::vector<TracePacket>* packets, bool has_more) {
        for (auto& encoded_packet : *packets) {
          protos::gen::TracePacket packet;
          ASSERT_TRUE(
              packet.ParseFromString(encoded_packet.GetRawBytesForTesting()));
          if (packet.has_for_testing()) {
            num_test_pack_rx++;
          }
        }
        if (!has_more)
          all_packets_rx();
      });
  task_runner_->RunUntilCheckpoint("all_packets_rx");
  ASSERT_EQ(3u, num_test_pack_rx);

  // Disable tracing.
  consumer_endpoint_->DisableTracing();

  auto on_tracing_disabled =
      task_runner_->CreateCheckpoint("on_tracing_disabled");
  auto on_stop_ds = task_runner_->CreateCheckpoint("on_stop_ds");
  EXPECT_CALL(producer_, StopDataSource(_))
      .WillOnce(InvokeWithoutArgs(on_stop_ds));
  EXPECT_CALL(consumer_, OnTracingDisabled(_))
      .WillOnce(InvokeWithoutArgs(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint("on_stop_ds");
  task_runner_->RunUntilCheckpoint("on_tracing_disabled");
}

// TODO(primiano): add tests to cover:
// - unknown fields preserved end-to-end.
// - >1 data source.
// - >1 data consumer sharing the same data source, with different TraceBuffers.
// - >1 consumer with > 1 buffer each.
// - Consumer disconnecting in the middle of a ReadBuffers() call.
// - Multiple calls to DisableTracing.
// - Out of order Enable/Disable/FreeBuffers calls.
// - DisableTracing does actually freeze the buffers.

}  // namespace perfetto
