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

#include <stdio.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/unix_task_runner.h"
#include "perfetto/ftrace_reader/ftrace_controller.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"

#include "protos/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/trace_packet.pbzero.h"

namespace perfetto {

namespace {

bool IsAlnum(const std::string& str) {
  for (size_t i = 0; i < str.size(); i++) {
    if (!isalnum(str[i]) && str[i] != '_')
      return false;
  }
  return true;
}

using BundleHandle =
    protozero::ProtoZeroMessageHandle<protos::pbzero::FtraceEventBundle>;

class SinkDelegate : public FtraceSink::Delegate {
 public:
  SinkDelegate(std::unique_ptr<TraceWriter> writer);
  ~SinkDelegate() override;

  // FtraceDelegateImpl
  BundleHandle GetBundleForCpu(size_t cpu) override;
  void OnBundleComplete(size_t cpu, BundleHandle bundle) override;

  void sink(std::unique_ptr<FtraceSink> sink) { sink_ = std::move(sink); }

 private:
  std::unique_ptr<FtraceSink> sink_ = nullptr;
  TraceWriter::TracePacketHandle trace_packet_;
  std::unique_ptr<TraceWriter> writer_;
};

SinkDelegate::SinkDelegate(std::unique_ptr<TraceWriter> writer)
    : writer_(std::move(writer)) {}

SinkDelegate::~SinkDelegate() = default;

BundleHandle SinkDelegate::GetBundleForCpu(size_t cpu) {
  trace_packet_ = writer_->NewTracePacket();
  return BundleHandle(trace_packet_->set_ftrace_events());
}

void SinkDelegate::OnBundleComplete(size_t cpu, BundleHandle bundle) {
  trace_packet_->Finalize();
}

class FtraceProducer : public Producer {
 public:
  ~FtraceProducer() override;

  // Producer Impl:
  void OnConnect() override;
  void OnDisconnect() override;
  void CreateDataSourceInstance(DataSourceInstanceID,
                                const DataSourceConfig&) override;
  void TearDownDataSourceInstance(DataSourceInstanceID) override;

  // Our Impl
  void Run();

 private:
  std::unique_ptr<Service::ProducerEndpoint> endpoint_ = nullptr;
  std::unique_ptr<FtraceController> ftrace_ = nullptr;
  DataSourceID data_source_id_ = 0;
  std::map<DataSourceInstanceID, std::unique_ptr<SinkDelegate>> delegates_;
};

FtraceProducer::~FtraceProducer() = default;

void FtraceProducer::OnConnect() {
  PERFETTO_LOG("Connected to the service\n");

  DataSourceDescriptor descriptor;
  descriptor.set_name("com.google.perfetto.ftrace");
  endpoint_->RegisterDataSource(
      descriptor, [this](DataSourceID id) { data_source_id_ = id; });
}

void FtraceProducer::OnDisconnect() {
  PERFETTO_LOG("Disconnected from tracing service");
  exit(1);
}

void FtraceProducer::CreateDataSourceInstance(
    DataSourceInstanceID id,
    const DataSourceConfig& source_config) {
  PERFETTO_LOG("Source start (id=%" PRIu64 ", target_buf=%" PRIu32 ")", id,
               source_config.target_buffer());

  // TODO(hjd): Would be nice if ftrace_reader could use generate the config.
  const DataSourceConfig::FtraceConfig proto_config =
      source_config.ftrace_config();

  FtraceConfig config;
  for (const std::string& event_name : proto_config.event_names()) {
    if (IsAlnum(event_name)) {
      config.AddEvent(event_name.c_str());
    } else {
      PERFETTO_LOG("Bad event name '%s'", event_name.c_str());
    }
  }

  // TODO(hjd): Static cast is bad, target_buffer() should return a BufferID.
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  auto delegate =
      std::unique_ptr<SinkDelegate>(new SinkDelegate(std::move(trace_writer)));
  auto sink = ftrace_->CreateSink(config, delegate.get());
  PERFETTO_CHECK(sink);
  delegate->sink(std::move(sink));
  delegates_.emplace(id, std::move(delegate));
}

void FtraceProducer::TearDownDataSourceInstance(DataSourceInstanceID id) {
  PERFETTO_LOG("Source stop (id=%" PRIu64 ")", id);
  delegates_.erase(id);
}

void FtraceProducer::Run() {
  base::UnixTaskRunner task_runner;
  ftrace_ = FtraceController::Create(&task_runner);
  endpoint_ = ProducerIPCClient::Connect(PERFETTO_PRODUCER_SOCK_NAME, this,
                                         &task_runner);
  ftrace_->DisableAllEvents();
  ftrace_->ClearTrace();
  task_runner.Run();
}

}  // namespace.

}  // namespace perfetto

namespace perfetto {

int __attribute__((visibility("default"))) ProbesMain(int argc, char** argv) {
  PERFETTO_LOG("Starting %s service", argv[0]);
  perfetto::FtraceProducer producer;
  producer.Run();
  return 0;
}

}  // namespace perfetto
