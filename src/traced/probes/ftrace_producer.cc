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

#include "src/traced/probes/ftrace_producer.h"

#include <stdio.h>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

bool IsGoodPunctuation(char c) {
  return c == '_' || c == '.';
}

uint64_t kInitialConnectionBackoffMs = 100;
uint64_t kMaxConnectionBackoffMs = 30 * 1000;

bool IsValid(const std::string& str) {
  for (size_t i = 0; i < str.size(); i++) {
    if (!isalnum(str[i]) && !IsGoodPunctuation(str[i]))
      return false;
  }
  return true;
}

}  // namespace.

// State transition diagram:
//                    +----------------------------+
//                    v                            +
// NotStarted -> NotConnected -> Connecting -> Connected
//                    ^              +
//                    +--------------+
//

FtraceProducer::~FtraceProducer() = default;

void FtraceProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service");

  DataSourceDescriptor descriptor;
  descriptor.set_name("com.google.perfetto.ftrace");
  endpoint_->RegisterDataSource(
      descriptor, [this](DataSourceID id) { data_source_id_ = id; });
}

void FtraceProducer::OnDisconnect() {
  PERFETTO_DCHECK(state_ == kConnected || state_ == kConnecting);
  state_ = kNotConnected;
  PERFETTO_LOG("Disconnected from tracing service");
  IncreaseConnectionBackoff();

  task_runner_->PostDelayedTask([this] { this->Connect(); },
                                connection_backoff_ms_);
}

void FtraceProducer::CreateDataSourceInstance(
    DataSourceInstanceID id,
    const DataSourceConfig& source_config) {
  PERFETTO_LOG("Ftrace start (id=%" PRIu64 ", target_buf=%" PRIu32 ")", id,
               source_config.target_buffer());

  // TODO(hjd): Would be nice if ftrace_reader could use the generated config.
  const DataSourceConfig::FtraceConfig proto_config =
      source_config.ftrace_config();

  FtraceConfig config;
  // TODO(b/72082266): We shouldn't have to do this.
  for (const std::string& event_name : proto_config.event_names()) {
    if (IsValid(event_name)) {
      config.AddEvent(event_name.c_str());
    } else {
      PERFETTO_ELOG("Bad event name '%s'", event_name.c_str());
    }
  }
  for (const std::string& category : proto_config.atrace_categories()) {
    if (IsValid(category)) {
      config.AddAtraceCategory(category.c_str());
    } else {
      PERFETTO_ELOG("Bad category name '%s'", category.c_str());
    }
  }
  for (const std::string& app : proto_config.atrace_apps()) {
    if (IsValid(app)) {
      config.AddAtraceApp(app.c_str());
    } else {
      PERFETTO_ELOG("Bad app '%s'", app.c_str());
    }
  }

  config.set_total_buffer_size_kb(proto_config.total_buffer_size_kb());
  config.set_drain_period_ms(proto_config.drain_period_ms());

  // TODO(hjd): Static cast is bad, target_buffer() should return a BufferID.
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  auto delegate =
      std::unique_ptr<SinkDelegate>(new SinkDelegate(std::move(trace_writer)));
  auto sink = ftrace_->CreateSink(std::move(config), delegate.get());
  PERFETTO_CHECK(sink);
  delegate->sink(std::move(sink));
  delegates_.emplace(id, std::move(delegate));
}

void FtraceProducer::TearDownDataSourceInstance(DataSourceInstanceID id) {
  PERFETTO_LOG("Ftrace stop (id=%" PRIu64 ")", id);
  delegates_.erase(id);
}

void FtraceProducer::ConnectWithRetries(const char* socket_name,
                                        base::TaskRunner* task_runner) {
  PERFETTO_DCHECK(state_ == kNotStarted);
  state_ = kNotConnected;

  ResetConnectionBackoff();
  socket_name_ = socket_name;
  task_runner_ = task_runner;
  ftrace_ = FtraceController::Create(task_runner);
  PERFETTO_CHECK(ftrace_);
  ftrace_->DisableAllEvents();
  ftrace_->ClearTrace();
  Connect();
}

void FtraceProducer::Connect() {
  PERFETTO_DCHECK(state_ == kNotConnected);
  state_ = kConnecting;
  endpoint_ = ProducerIPCClient::Connect(socket_name_, this, task_runner_);
}

void FtraceProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void FtraceProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

FtraceProducer::SinkDelegate::SinkDelegate(std::unique_ptr<TraceWriter> writer)
    : writer_(std::move(writer)) {}

FtraceProducer::SinkDelegate::~SinkDelegate() = default;

FtraceProducer::BundleHandle FtraceProducer::SinkDelegate::GetBundleForCpu(
    size_t cpu) {
  trace_packet_ = writer_->NewTracePacket();
  return BundleHandle(trace_packet_->set_ftrace_events());
}

void FtraceProducer::SinkDelegate::OnBundleComplete(size_t cpu,
                                                    BundleHandle bundle) {
  trace_packet_->Finalize();
}

}  // namespace perfetto
