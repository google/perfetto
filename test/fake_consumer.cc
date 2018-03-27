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

#include "test/fake_consumer.h"

#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"

namespace perfetto {

FakeConsumer::FakeConsumer(
    const TraceConfig& trace_config,
    std::function<void()> on_connect,
    std::function<void(std::vector<TracePacket>, bool)> packet_callback,
    base::TaskRunner* task_runner)
    : task_runner_(task_runner),
      trace_config_(trace_config),
      on_connect_(on_connect),
      packet_callback_(std::move(packet_callback)) {}
FakeConsumer::~FakeConsumer() = default;

void FakeConsumer::Connect(const char* socket_name) {
  endpoint_ = ConsumerIPCClient::Connect(socket_name, this, task_runner_);
}

void FakeConsumer::Disconnect() {
  endpoint_.reset();
}

void FakeConsumer::OnConnect() {
  on_connect_();
}

void FakeConsumer::EnableTracing() {
  endpoint_->EnableTracing(trace_config_);
}

void FakeConsumer::FreeBuffers() {
  endpoint_->FreeBuffers();
}

void FakeConsumer::ReadTraceData() {
  endpoint_->ReadBuffers();
}

void FakeConsumer::OnDisconnect() {
  FAIL() << "Consumer unexpectedly disconnected from the service";
}

void FakeConsumer::OnTraceData(std::vector<TracePacket> data, bool has_more) {
  packet_callback_(std::move(data), has_more);
}

}  // namespace perfetto
