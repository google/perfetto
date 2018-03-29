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
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "test/task_runner_thread_delegates.h"

namespace perfetto {

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#define TEST_CONSUMER_SOCK_NAME "/data/local/tmp/traced_consumer"
#else
#define TEST_PRODUCER_SOCK_NAME PERFETTO_PRODUCER_SOCK_NAME
#define TEST_CONSUMER_SOCK_NAME PERFETTO_CONSUMER_SOCK_NAME
#endif

TestHelper::TestHelper(base::TestTaskRunner* task_runner)
    : task_runner_(task_runner),
      service_thread_("perfetto.svc"),
      producer_thread_("perfetto.prd") {}

void TestHelper::OnConnect() {
  std::move(continuation_callack_)();
}

void TestHelper::OnDisconnect() {
  FAIL() << "Consumer unexpectedly disconnected from the service";
}

void TestHelper::OnTracingStop() {}

void TestHelper::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  for (auto& packet : packets) {
    ASSERT_TRUE(packet.Decode());
    if (packet->has_clock_snapshot() || packet->has_trace_config())
      continue;
    ASSERT_EQ(protos::TracePacket::kTrustedUid,
              packet->optional_trusted_uid_case());
    packet_callback_(*packet);
  }

  if (!has_more) {
    packet_callback_ = {};
    std::move(continuation_callack_)();
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
      new FakeProducerDelegate(
          TEST_PRODUCER_SOCK_NAME,
          WrapTask(task_runner_->CreateCheckpoint("producer.enabled"))));
  FakeProducerDelegate* producer_delegate_cached = producer_delegate.get();
  producer_thread_.Start(std::move(producer_delegate));
  return producer_delegate_cached->producer();
}

void TestHelper::ConnectConsumer() {
  continuation_callack_ = task_runner_->CreateCheckpoint("consumer.connected");
  endpoint_ =
      ConsumerIPCClient::Connect(TEST_CONSUMER_SOCK_NAME, this, task_runner_);
  task_runner_->RunUntilCheckpoint("consumer.connected");
}

void TestHelper::StartTracing(const TraceConfig& config) {
  endpoint_->EnableTracing(config);
  task_runner_->RunUntilCheckpoint("producer.enabled");
}

void TestHelper::ReadData(
    std::function<void(const TracePacket::DecodedTracePacket&)> packet_callback,
    std::function<void()> on_finish_callback) {
  packet_callback_ = packet_callback;
  continuation_callack_ = on_finish_callback;
  endpoint_->ReadBuffers();
}

std::function<void()> TestHelper::WrapTask(
    const std::function<void()>& function) {
  return [this, function] { task_runner_->PostTask(function); };
}

}  // namespace perfetto
