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

#include "src/tracing/test/mock_consumer.h"

#include "perfetto/tracing/core/trace_config.h"
#include "src/base/test/test_task_runner.h"

using ::testing::_;
using ::testing::Invoke;

namespace perfetto {

MockConsumer::MockConsumer(base::TestTaskRunner* task_runner)
    : task_runner_(task_runner) {}

MockConsumer::~MockConsumer() {
  if (!service_endpoint_)
    return;
  static int i = 0;
  auto checkpoint_name = "on_consumer_disconnect_" + std::to_string(i++);
  auto on_disconnect = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnDisconnect()).WillOnce(Invoke(on_disconnect));
  service_endpoint_.reset();
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

void MockConsumer::Connect(TracingService* svc) {
  service_endpoint_ = svc->ConnectConsumer(this);
  static int i = 0;
  auto checkpoint_name = "on_consumer_connect_" + std::to_string(i++);
  auto on_connect = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnConnect()).WillOnce(Invoke(on_connect));
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

void MockConsumer::EnableTracing(const TraceConfig& trace_config,
                                 base::ScopedFile write_into_file) {
  service_endpoint_->EnableTracing(trace_config, std::move(write_into_file));
}

void MockConsumer::DisableTracing() {
  service_endpoint_->DisableTracing();
}

void MockConsumer::FreeBuffers() {
  service_endpoint_->FreeBuffers();
}

void MockConsumer::WaitForTracingDisabled() {
  static int i = 0;
  auto checkpoint_name = "on_tracing_disabled_consumer_" + std::to_string(i++);
  auto on_tracing_disabled = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnTracingDisabled()).WillOnce(Invoke(on_tracing_disabled));
  task_runner_->RunUntilCheckpoint(checkpoint_name);
}

MockConsumer::FlushRequest MockConsumer::Flush(uint32_t timeout_ms) {
  static int i = 0;
  auto checkpoint_name = "on_consumer_flush_" + std::to_string(i++);
  auto on_flush = task_runner_->CreateCheckpoint(checkpoint_name);
  std::shared_ptr<bool> result(new bool());
  service_endpoint_->Flush(timeout_ms, [result, on_flush](bool success) {
    *result = success;
    on_flush();
  });

  base::TestTaskRunner* task_runner = task_runner_;
  auto wait_for_flush_completion = [result, task_runner,
                                    checkpoint_name]() -> bool {
    task_runner->RunUntilCheckpoint(checkpoint_name);
    return *result;
  };

  return FlushRequest(wait_for_flush_completion);
}

std::vector<protos::TracePacket> MockConsumer::ReadBuffers() {
  std::vector<protos::TracePacket> decoded_packets;
  static int i = 0;
  std::string checkpoint_name = "on_read_buffers_" + std::to_string(i++);
  auto on_read_buffers = task_runner_->CreateCheckpoint(checkpoint_name);
  EXPECT_CALL(*this, OnTraceData(_, _))
      .WillRepeatedly(
          Invoke([&decoded_packets, on_read_buffers](
                     std::vector<TracePacket>* packets, bool has_more) {
            for (TracePacket& packet : *packets) {
              decoded_packets.emplace_back();
              protos::TracePacket* decoded_packet = &decoded_packets.back();
              packet.Decode(decoded_packet);
            }
            if (!has_more)
              on_read_buffers();
          }));
  service_endpoint_->ReadBuffers();
  task_runner_->RunUntilCheckpoint(checkpoint_name);
  return decoded_packets;
}

}  // namespace perfetto
