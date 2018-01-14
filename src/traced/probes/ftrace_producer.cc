#include "ftrace_producer.h"

#include <stdio.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/unix_task_runner.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"

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

}  // namespace.

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
