/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/null_consumer_endpoint_for_testing.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/tracing_service_state.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::Property;
using ::testing::SizeIs;

}  // namespace

TEST(PerfettoTracedIntegrationTest, NullConsumerEndpointBuilds) {
  NullConsumerEndpointForTesting npe;
  npe.StartTracing();
}

TEST(PerfettoTracedIntegrationTest, TestFakeProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 12;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
  }
}

TEST(PerfettoTracedIntegrationTest, VeryLargePackets) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(500);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 7;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024 * 1024 - 42;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData(/* read_count */ 0, /* timeout_ms */ 10000);

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
    size_t msg_size = packet.for_testing().str().size();
    ASSERT_EQ(kMsgSize, msg_size);
    for (size_t i = 0; i < msg_size; i++)
      ASSERT_EQ(i < msg_size - 1 ? '.' : 0, packet.for_testing().str()[i]);
  }
}

// This is a regression test see b/169051440 for context.
//
// In this test we ensure that traced will not crash if a Producer stops
// responding or draining the socket (i.e. after we fill up the IPC buffer
// traced doesn't block on trying to write to the IPC buffer and watchdog
// doesn't kill it).
TEST(PerfettoTracedIntegrationTest, UnresponsiveProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  auto* producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(100);
  trace_config.set_flush_timeout_ms(1);
  trace_config.set_data_source_stop_timeout_ms(1);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");

  static constexpr size_t kNumPackets = 1;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024 * 1024 - 42;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  // This string is just used to make the StartDataSource IPC larger.
  ds_config->set_legacy_config(std::string(8192, '.'));
  ds_config->set_target_buffer(0);

  // Run one legit trace, this ensures that the producer above is
  // valid and correct and mirrors real life producers.
  helper.StartTracing(trace_config);
  helper.WaitForProducerEnabled();
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData(/* read_count */ 0, /* timeout_ms */ 10000);

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), 1u);
  ASSERT_TRUE(packets[0].has_for_testing());
  ASSERT_FALSE(packets[0].for_testing().str().empty());
  helper.FreeBuffers();

  // Switch the producer to ignoring the IPC socket. On a pixel 4 it took 13
  // traces to fill up the IPC buffer and cause traced to block (and eventually
  // watchdog to kill it).
  helper.producer_thread()->get()->RemoveFileDescriptorWatch(
      producer->unix_socket_fd());

  trace_config.set_duration_ms(1);
  for (uint32_t i = 0u; i < 15u; i++) {
    helper.StartTracing(trace_config, base::ScopedFile());
    helper.WaitForTracingDisabled(/* timeout_ms = */ 20000);
    helper.FreeBuffers();
  }
  // We need to readd the FileDescriptor (otherwise when the UnixSocket attempts
  // to remove it a the FakeProducer is destroyed will hit a CHECK failure.
  helper.producer_thread()->get()->AddFileDescriptorWatch(
      producer->unix_socket_fd(), []() {});
}

TEST(PerfettoTracedIntegrationTest, DetachAndReattach) {
  base::TestTaskRunner task_runner;

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(10000);  // Max timeout, session is ended before.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  static constexpr size_t kNumPackets = 11;
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(32);

  // Enable tracing and detach as soon as it gets started.
  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  auto* fake_producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  helper.StartTracing(trace_config);

  // Detach.
  helper.DetachConsumer("key");

  // Write data while detached.
  helper.WaitForProducerEnabled();
  auto on_data_written = task_runner.CreateCheckpoint("data_written");
  fake_producer->ProduceEventBatch(helper.WrapTask(on_data_written));
  task_runner.RunUntilCheckpoint("data_written");

  // Then reattach the consumer.
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  helper.AttachConsumer("key");

  helper.DisableTracing();
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();
  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);
}

// Tests that a detached trace session is automatically cleaned up if the
// consumer doesn't re-attach before its expiration time.
TEST(PerfettoTracedIntegrationTest, ReattachFailsAfterTimeout) {
  base::TestTaskRunner task_runner;

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(250);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(100000);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(1);
  ds_config->mutable_for_testing()->set_message_size(32);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  // Enable tracing and detach as soon as it gets started.
  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  auto pipe_pair = base::Pipe::Create();
  helper.StartTracing(trace_config, std::move(pipe_pair.wr));

  // Detach.
  helper.DetachConsumer("key");

  // Use the file EOF (write end closed) as a way to detect when the trace
  // session is ended.
  char buf[1024];
  while (PERFETTO_EINTR(read(*pipe_pair.rd, buf, sizeof(buf))) > 0) {
  }

  // Give some margin for the tracing service to destroy the session.
  usleep(250000);

  // Reconnect and find out that it's too late and the session is gone.
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  EXPECT_FALSE(helper.AttachConsumer("key"));
}

TEST(PerfettoTracedIntegrationTest, TestProducerProvidedSMB) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.CreateProducerProvidedSmb();

  protos::gen::TestConfig test_config;
  test_config.set_seed(42);
  test_config.set_message_count(1);
  test_config.set_message_size(1024);
  test_config.set_send_batch_on_register(true);

  // Write a first batch before connection.
  helper.ProduceStartupEventBatch(test_config);

  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);
  *ds_config->mutable_for_testing() = test_config;

  // The data source is configured to emit another batch when it is started via
  // send_batch_on_register in the TestConfig.
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  EXPECT_TRUE(helper.IsShmemProvidedByProducer());

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  // We should have produced two batches, one before the producer connected and
  // another one when the data source was started.
  ASSERT_EQ(packets.size(), 2u);
  ASSERT_TRUE(packets[0].has_for_testing());
  ASSERT_TRUE(packets[1].has_for_testing());
}

// Regression test for b/153142114.
TEST(PerfettoTracedIntegrationTest, QueryServiceStateLargeResponse) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  FakeProducer* producer = helper.ConnectFakeProducer();

  // Register 5 data sources with very large descriptors. Each descriptor will
  // max out the IPC message size, so that the service has no other choice
  // than chunking them.
  std::map<std::string, std::string> ds_expected;
  for (int i = 0; i < 5; i++) {
    DataSourceDescriptor dsd;
    std::string name = "big_ds_" + std::to_string(i);
    dsd.set_name(name);
    std::string descriptor(ipc::kIPCBufferSize - 64,
                           static_cast<char>((' ' + i) % 64));
    dsd.set_track_event_descriptor_raw(descriptor);
    ds_expected[name] = std::move(descriptor);
    producer->RegisterDataSource(dsd);
  }

  // Linearize the producer with the service. We need to make sure that all the
  // RegisterDataSource() calls above have been seen by the service before
  // continuing.
  helper.SyncAndWaitProducer();

  // Now invoke QueryServiceState() and wait for the reply. The service will
  // send 6 (1 + 5) IPCs which will be merged together in
  // producer_ipc_client_impl.cc.
  auto svc_state = helper.QueryServiceStateAndWait();

  ASSERT_GE(svc_state.producers().size(), 1u);

  std::map<std::string, std::string> ds_found;
  for (const auto& ds : svc_state.data_sources()) {
    if (!base::StartsWith(ds.ds_descriptor().name(), "big_ds_"))
      continue;
    ds_found[ds.ds_descriptor().name()] =
        ds.ds_descriptor().track_event_descriptor_raw();
  }
  EXPECT_THAT(ds_found, ElementsAreArray(ds_expected));
}

// Regression test for b/195065199. Check that trace filtering works when a
// packet size exceeds the IPC limit. This tests that the tracing service, when
// reassembling packets after filtering, doesn't "overglue" them. They still
// need to be slice-able to fit into the ReadBuffers ipc.
TEST(PerfettoTracedIntegrationTest, TraceFilterLargePackets) {
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);

  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024 * 16);
  trace_config.set_duration_ms(500);
  auto* prod_config = trace_config.add_producers();
  prod_config->set_producer_name("android.perfetto.FakeProducer");
  prod_config->set_shm_size_kb(1024 * 16);
  prod_config->set_page_size_kb(32);

  static constexpr size_t kNumPackets = 3;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 8 * ipc::kIPCBufferSize;
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  auto* test_config = ds_config->mutable_for_testing();
  test_config->set_seed(kRandomSeed);
  test_config->set_message_count(kNumPackets);
  test_config->set_message_size(kMsgSize);
  test_config->set_send_batch_on_register(true);

  protozero::FilterBytecodeGenerator filt;
  // Message 0: root Trace proto.
  filt.AddNestedField(1 /* root trace.packet*/, 1);
  filt.EndMessage();

  // Message 1: TracePacket proto. Allow all fields.
  filt.AddSimpleFieldRange(1, 1000);
  filt.EndMessage();

  trace_config.mutable_trace_filter()->set_bytecode(filt.Serialize());

  // The data source is configured to emit another batch when it is started via
  // send_batch_on_register in the TestConfig.
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData(/* read_count */ 0, /* timeout_ms */ 10000);

  const std::vector<protos::gen::TracePacket>& packets = helper.trace();
  EXPECT_EQ(packets.size(), kNumPackets);
  EXPECT_THAT(packets,
              Each(Property(&protos::gen::TracePacket::has_for_testing, true)));
  EXPECT_THAT(
      packets,
      Each(Property(&protos::gen::TracePacket::for_testing,
                    Property(&protos::gen::TestEvent::str, SizeIs(kMsgSize)))));
}

#if (PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS) && \
     PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)
TEST(PerfettoTracedIntegrationTest, TestMultipleProducerSockets) {
  base::TestTaskRunner task_runner;
  auto temp_dir = base::TempDir::Create();

  std::vector<std::string> producer_socket_names{
      temp_dir.path() + "/producer1.sock",
      temp_dir.path() + "/producer2.sock",
  };
  auto producer_sock_name = base::Join(producer_socket_names, ",");
  // We need to start the service thread for multiple producer sockets.
  TestHelper helper(&task_runner, TestHelper::Mode::kStartDaemons,
                    producer_sock_name.c_str());
  ASSERT_EQ(helper.num_producers(), 2u);
  helper.StartServiceIfRequired();
  // Setup the 1st producer (default).
  helper.ConnectFakeProducer();
  // Setup the 2ns producer.
  helper.ConnectFakeProducer(1);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  static constexpr uint32_t kMsgSize = 1024;
  // Enable the 1st producer.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);
  ds_config->mutable_for_testing()->set_message_count(12);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);
  // Enable the 2nd producer.
  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer.1");
  ds_config->set_target_buffer(0);
  ds_config->mutable_for_testing()->set_message_count(24);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), 36u);

  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
  }

  for (const auto& sock_name : producer_socket_names)
    remove(sock_name.c_str());
}

TEST(PerfettoTracedIntegrationTest, TestShmemEmulation) {
  base::TestTaskRunner task_runner;
  auto temp_dir = base::TempDir::Create();

  std::string sock_name;
  {
    // Set up a server UnixSocket to find an unused TCP port.
    base::UnixSocket::EventListener event_listener;
    auto srv = base::UnixSocket::Listen("127.0.0.1:0", &event_listener,
                                        &task_runner, base::SockFamily::kInet,
                                        base::SockType::kStream);
    ASSERT_TRUE(srv->is_listening());
    sock_name = srv->GetSockAddr();
    // Shut down |srv| here to free the port. It's unlikely that the port will
    // be taken by another process so quickly before we reach the code below.
  }

  TestHelper helper(&task_runner, TestHelper::Mode::kStartDaemons,
                    sock_name.c_str());
  ASSERT_EQ(helper.num_producers(), 1u);
  helper.StartServiceIfRequired();
  // Setup the 1st producer (default).
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  static constexpr uint32_t kMsgSize = 1024;
  static constexpr uint32_t kRandomSeed = 42;
  // Enable the producer.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(12);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), 12u);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
  }
}
#endif

}  // namespace perfetto
