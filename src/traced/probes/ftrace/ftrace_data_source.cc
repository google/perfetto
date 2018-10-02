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

#include "src/traced/probes/ftrace/ftrace_data_source.h"

#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

// static
constexpr int FtraceDataSource::kTypeId;

FtraceDataSource::FtraceDataSource(
    base::WeakPtr<FtraceController> controller_weak,
    TracingSessionID session_id,
    const FtraceConfig& config,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, kTypeId),
      config_(config),
      writer_(std::move(writer)),
      controller_weak_(std::move(controller_weak)){};

FtraceDataSource::~FtraceDataSource() {
  if (controller_weak_)
    controller_weak_->RemoveDataSource(this);
};

void FtraceDataSource::Initialize(FtraceConfigId config_id,
                                  std::unique_ptr<EventFilter> event_filter) {
  PERFETTO_CHECK(config_id);
  config_id_ = config_id;
  event_filter_ = std::move(event_filter);
}

void FtraceDataSource::Start() {
  FtraceController* ftrace = controller_weak_.get();
  if (!ftrace)
    return;
  PERFETTO_CHECK(config_id_);  // Must be initialized at this point.
  if (!ftrace->StartDataSource(this))
    return;
  DumpFtraceStats(&stats_before_);
}

void FtraceDataSource::DumpFtraceStats(FtraceStats* stats) {
  if (controller_weak_)
    controller_weak_->DumpFtraceStats(stats);
}

void FtraceDataSource::Flush() {
  // TODO(primiano): this still doesn't flush data from the kernel ftrace
  // buffers (see b/73886018). We should do that and delay the
  // NotifyFlushComplete() until the ftrace data has been drained from the
  // kernel ftrace buffer and written in the SMB.
  if (!writer_)
    return;
  WriteStats();
  writer_->Flush();
}

void FtraceDataSource::WriteStats() {
  {
    auto before_packet = writer_->NewTracePacket();
    auto out = before_packet->set_ftrace_stats();
    out->set_phase(protos::pbzero::FtraceStats_Phase_START_OF_TRACE);
    stats_before_.Write(out);
  }
  {
    FtraceStats stats_after{};
    DumpFtraceStats(&stats_after);
    auto after_packet = writer_->NewTracePacket();
    auto out = after_packet->set_ftrace_stats();
    out->set_phase(protos::pbzero::FtraceStats_Phase_END_OF_TRACE);
    stats_after.Write(out);
  }
}

}  // namespace perfetto
