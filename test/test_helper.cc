/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "test/test_helper.h"

#include "gtest/gtest.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "test/task_runner_thread_delegates.h"

#include "src/tracing/ipc/default_socket.h"

#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

uint64_t TestHelper::next_instance_num_ = 0;

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#define TEST_CONSUMER_SOCK_NAME "/data/local/tmp/traced_consumer"
#else
#define TEST_PRODUCER_SOCK_NAME ::perfetto::GetProducerSocket()
#define TEST_CONSUMER_SOCK_NAME ::perfetto::GetConsumerSocket()
#endif

TestHelper::TestHelper(base::TestTaskRunner* task_runner)
    : instance_num_(next_instance_num_++),
      task_runner_(task_runner),
      service_thread_("perfetto.svc"),
      producer_thread_("perfetto.prd") {}

void TestHelper::OnConnect() {
  std::move(on_connect_callback_)();
}

void TestHelper::OnDisconnect() {
  FAIL() << "Consumer unexpectedly disconnected from the service";
}

void TestHelper::OnTracingDisabled() {
  std::move(on_stop_tracing_callback_)();
}

void TestHelper::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  for (auto& encoded_packet : packets) {
    protos::TracePacket packet;
    ASSERT_TRUE(encoded_packet.Decode(&packet));
    if (packet.has_clock_snapshot() || packet.has_trace_config() ||
        packet.has_trace_stats() || !packet.synchronization_marker().empty() ||
        packet.has_system_info()) {
      continue;
    }
    ASSERT_EQ(protos::TracePacket::kTrustedUid,
              packet.optional_trusted_uid_case());
    trace_.push_back(std::move(packet));
  }

  if (!has_more) {
    std::move(on_packets_finished_callback_)();
  }
}

void TestHelper::StartServiceIfRequired() {
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  service_thread_.Start(std::unique_ptr<ServiceDelegate>(
      new ServiceDelegate(TEST_PRODUCER_SOCK_NAME, TEST_CONSUMER_SOCK_NAME)));
#endif
}

FakeProducer* TestHelper::ConnectFakeProducer() {
  std::unique_ptr<FakeProducerDelegate> producer_delegate(
      new FakeProducerDelegate(TEST_PRODUCER_SOCK_NAME,
                               WrapTask(CreateCheckpoint("producer.enabled"))));
  FakeProducerDelegate* producer_delegate_cached = producer_delegate.get();
  producer_thread_.Start(std::move(producer_delegate));
  return producer_delegate_cached->producer();
}

void TestHelper::ConnectConsumer() {
  cur_consumer_num_++;
  on_connect_callback_ = CreateCheckpoint("consumer.connected." +
                                          std::to_string(cur_consumer_num_));
  endpoint_ =
      ConsumerIPCClient::Connect(TEST_CONSUMER_SOCK_NAME, this, task_runner_);
}

void TestHelper::DetachConsumer(const std::string& key) {
  on_detach_callback_ = CreateCheckpoint("detach." + key);
  endpoint_->Detach(key);
  RunUntilCheckpoint("detach." + key);
  endpoint_.reset();
}

bool TestHelper::AttachConsumer(const std::string& key) {
  bool success = false;
  auto checkpoint = CreateCheckpoint("attach." + key);
  on_attach_callback_ = [&success, checkpoint](bool s) {
    success = s;
    checkpoint();
  };
  endpoint_->Attach(key);
  RunUntilCheckpoint("attach." + key);
  return success;
}

void TestHelper::StartTracing(const TraceConfig& config,
                              base::ScopedFile file) {
  trace_.clear();
  on_stop_tracing_callback_ = CreateCheckpoint("stop.tracing");
  endpoint_->EnableTracing(config, std::move(file));
}

void TestHelper::DisableTracing() {
  endpoint_->DisableTracing();
}

void TestHelper::FlushAndWait(uint32_t timeout_ms) {
  static int flush_num = 0;
  std::string checkpoint_name = "flush." + std::to_string(flush_num++);
  auto checkpoint = CreateCheckpoint(checkpoint_name);
  endpoint_->Flush(timeout_ms, [checkpoint](bool) { checkpoint(); });
  RunUntilCheckpoint(checkpoint_name, timeout_ms + 1000);
}

void TestHelper::ReadData(uint32_t read_count) {
  on_packets_finished_callback_ =
      CreateCheckpoint("readback.complete." + std::to_string(read_count));
  endpoint_->ReadBuffers();
}

void TestHelper::WaitForConsumerConnect() {
  RunUntilCheckpoint("consumer.connected." + std::to_string(cur_consumer_num_));
}

void TestHelper::WaitForProducerEnabled() {
  RunUntilCheckpoint("producer.enabled");
}

void TestHelper::WaitForTracingDisabled(uint32_t timeout_ms) {
  RunUntilCheckpoint("stop.tracing", timeout_ms);
}

void TestHelper::WaitForReadData(uint32_t read_count) {
  RunUntilCheckpoint("readback.complete." + std::to_string(read_count));
}

std::function<void()> TestHelper::WrapTask(
    const std::function<void()>& function) {
  return [this, function] { task_runner_->PostTask(function); };
}

void TestHelper::OnDetach(bool) {
  if (on_detach_callback_)
    std::move(on_detach_callback_)();
}

void TestHelper::OnAttach(bool success, const TraceConfig&) {
  if (on_attach_callback_)
    std::move(on_attach_callback_)(success);
}

void TestHelper::OnTraceStats(bool, const TraceStats&) {}

void TestHelper::OnObservableEvents(const ObservableEvents&) {}

// static
const char* TestHelper::GetConsumerSocketName() {
  return TEST_CONSUMER_SOCK_NAME;
}

}  // namespace perfetto
