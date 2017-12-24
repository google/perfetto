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

#include <inttypes.h>

#include "perfetto/base/unix_task_runner.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"

#include "protos/trace_packet.pb.h"

namespace perfetto {
namespace {

class ConsumerCmd : Consumer {
 public:
  explicit ConsumerCmd(base::TaskRunner*, TraceConfig);
  ~ConsumerCmd() override;

  void OnTraceTimer();

  // Consumer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTraceData(std::vector<TracePacket>, bool has_more) override;

 private:
  base::TaskRunner* task_runner_;
  TraceConfig trace_config_;
  std::unique_ptr<Service::ConsumerEndpoint> consumer_endpoint_;
};

ConsumerCmd::ConsumerCmd(base::TaskRunner* task_runner,
                         TraceConfig trace_config)
    : task_runner_(task_runner),
      trace_config_(std::move(trace_config)),
      consumer_endpoint_(ConsumerIPCClient::Connect(PERFETTO_CONSUMER_SOCK_NAME,
                                                    this,
                                                    task_runner)) {}

ConsumerCmd::~ConsumerCmd() = default;

void ConsumerCmd::OnConnect() {
  PERFETTO_ILOG("Connected to tracing service, enabling tracing");
  consumer_endpoint_->EnableTracing(trace_config_);
  // TODO(primiano): auto-disabling should be really up to the tracing service,
  // move this responsibility there.
  task_runner_->PostDelayedTask(std::bind(&ConsumerCmd::OnTraceTimer, this),
                                trace_config_.duration_ms());
}

void ConsumerCmd::OnTraceTimer() {
  PERFETTO_ILOG("Timer expired, disabling timer");
  consumer_endpoint_->DisableTracing();
  consumer_endpoint_->ReadBuffers();
}

void ConsumerCmd::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  for (TracePacket& packet : packets) {
    bool decoded = packet.Decode();
    PERFETTO_ILOG("Received packet decoded: %d size: %zu", decoded,
                  packet.size());
  }

  if (!has_more) {
    consumer_endpoint_->FreeBuffers();
    task_runner_->PostTask([] { _exit(0); });
  }
}

void ConsumerCmd::OnDisconnect() {
  PERFETTO_ILOG("Disconnected from tracing service");
}

}  // namespace
}  // namespace perfetto

int main() {
  // Prepare trace config.
  // TODO: this should read the text-version protobuf from stdin using
  // libprotobuf_full.
  perfetto::TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(10000);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("perfetto.test");
  ds_config->set_target_buffer(0);
  ds_config->set_trace_category_filters("foo,bar");

  perfetto::base::UnixTaskRunner task_runner;
  perfetto::ConsumerCmd consumer(&task_runner, std::move(trace_config));

  task_runner.Run();
  return 0;
}
