/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <mutex>

#include "protos/perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "src/profiling/perf/ftrace_event_manager.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/tracefs.h"

namespace perfetto {
namespace profiling {

namespace {

perfetto::protos::pbzero::FtraceClock ToFtraceClock(int32_t clock_id) {
  using perfetto::protos::pbzero::FtraceClock;
  switch (clock_id) {
    case CLOCK_REALTIME:
      return FtraceClock::FTRACE_CLOCK_REALTIME;
    case CLOCK_MONOTONIC:
      return FtraceClock::FTRACE_CLOCK_MONO;
    case CLOCK_MONOTONIC_RAW:
      return FtraceClock::FTRACE_CLOCK_MONO_RAW;
    case CLOCK_BOOTTIME:
      return FtraceClock::FTRACE_CLOCK_UNSPECIFIED;
    default:
      return FtraceClock::FTRACE_CLOCK_UNSPECIFIED;
  }
}

using perfetto::protos::pbzero::FtraceEvent;
using protos::pbzero::FtraceParseStatus;

}  // namespace

FtraceEventManager::FtraceEventManager(
    perfetto::ProtoTranslationTable* table,
    const EventConfig* event_config,
    TraceWriter* trace_writer,
    bool compact_sched_enabled,
    perfetto::TracingService::ProducerEndpoint* endpoint,
    perfetto::BufferID buffer_id)
    : compact_sched_enabled_(compact_sched_enabled),
      event_config_(event_config),
      translation_table_(table),
      endpoint_(endpoint),
      buffer_id_(buffer_id),
      trace_writer_(trace_writer),
      ds_config_(
          /*event_filter=*/EventFilter{},
          /*syscall_filter=*/EventFilter{},
          CompactSchedConfig(compact_sched_enabled),
          /*print_filter=*/std::nullopt,
          /*atrace_apps=*/{},
          /*atrace_categories=*/{},
          /*atrace_categories_prefer_track_event=*/{},
          /*symbolize_ksyms=*/false,
          /*preserve_ftrace_buffer=*/false,
          /*syscalls_returning_fd=*/{},
          /*kprobes=*/
          base::FlatHashMap<uint32_t, protos::pbzero::KprobeEvent::KprobeType>{
              0},
          /*debug_ftrace_abi=*/false,
          /*write_generic_evt_descriptors=*/false) {
  PERFETTO_CHECK(event_config_);
  PERFETTO_CHECK(translation_table_);
  PERFETTO_DCHECK(endpoint_);
  for (auto& event : translation_table_->events()) {
    ds_config_.event_filter.AddEnabledEvent(event.ftrace_event_id);
  }
}

FtraceEventManager::Bundler* FtraceEventManager::GetOrCreateBundler(
    uint32_t cpu) {
  auto& bundler = cpu_bundler_map_[cpu];
  if (!bundler) {
    auto writer =
        endpoint_->CreateTraceWriter(buffer_id_, BufferExhaustedPolicy::kStall);
    bundler = std::make_unique<FtraceEventManager::Bundler>(
        std::move(writer), cpu, event_config_->perf_attr()->clockid, &metadata_,
        translation_table_->generic_evt_pb_descriptors(),
        compact_sched_enabled_);
  }
  return bundler.get();
}

FtraceParseStatus FtraceEventManager::ProcessSample(
    const ParsedSample& sample) {
  if (sample.raw_data.empty()) {
    return FtraceParseStatus::FTRACE_STATUS_UNSPECIFIED;
  }

  uint16_t ftrace_event_id = 0;
  memcpy(reinterpret_cast<void*>(&ftrace_event_id),
         reinterpret_cast<const void*>(sample.raw_data.data()),
         sizeof(ftrace_event_id));

  const perfetto::Event* event =
      translation_table_->GetEventById(ftrace_event_id);
  if (!event) {
    PERFETTO_DLOG("FtraceEventManager: Event (id=%u) not mapped",
                  ftrace_event_id);
    auto status = FtraceParseStatus::FTRACE_STATUS_INVALID_EVENT;
    WriteAndSetParseError(GetOrCreateBundler(sample.common.cpu),
                          sample.common.timestamp, status);
    return status;
  }

  FtraceParseStatus status;
  if (compact_sched_enabled_ &&
      event->proto_field_id == FtraceEvent::kSchedSwitchFieldNumber) {
    status = ParseSchedSwitchCompact(sample);
  } else if (compact_sched_enabled_ &&
             event->proto_field_id == FtraceEvent::kSchedWakingFieldNumber) {
    status = ParseSchedWakingCompact(sample);
  } else {
    status = ParseFtraceEvent(sample, ftrace_event_id);
  }
  if (status != FtraceParseStatus::FTRACE_STATUS_OK) {
    WriteAndSetParseError(GetOrCreateBundler(sample.common.cpu),
                          sample.common.timestamp, status);
  }
  return status;
}

FtraceParseStatus FtraceEventManager::ParseFtraceEvent(
    const ParsedSample& sample,
    uint16_t event_id) {
  auto bundler = GetOrCreateBundler(sample.common.cpu);
  auto event = bundler->AddEvent();
  event->set_timestamp(sample.common.timestamp);
  auto& raw = sample.raw_data;

  if (!CpuReader::ParseEvent(event_id, raw.data(), raw.data() + raw.size(),
                             translation_table_, &ds_config_, event, &metadata_,
                             bundler->generic_descriptors_to_write())) {
    return protos::pbzero::FtraceParseStatus::FTRACE_STATUS_INVALID_EVENT;
  }
  bundler->SetLastEventTimestamp(sample.common.timestamp);
  return FtraceParseStatus::FTRACE_STATUS_OK;
}

FtraceParseStatus FtraceEventManager::ParseSchedSwitchCompact(
    const ParsedSample& sample) {
  const CompactSchedSwitchFormat& sched_switch_format =
      translation_table_->compact_sched_format().sched_switch;

  if (sample.raw_data.size() < sched_switch_format.size) {
    return FtraceParseStatus::FTRACE_STATUS_SHORT_COMPACT_EVENT;
  }
  auto bundler = GetOrCreateBundler(sample.common.cpu);
  CpuReader::ParseSchedSwitchCompact(
      sample.raw_data.data(), sample.common.timestamp, &sched_switch_format,
      bundler->CompactSchedBuf(), &metadata_);

  bundler->SetLastEventTimestamp(sample.common.timestamp);
  bundler->MaybeFinalize();
  return FtraceParseStatus::FTRACE_STATUS_OK;
}

FtraceParseStatus FtraceEventManager::ParseSchedWakingCompact(
    const ParsedSample& sample) {
  const CompactSchedWakingFormat& sched_waking_format =
      translation_table_->compact_sched_format().sched_waking;

  if (sample.raw_data.size() < sched_waking_format.size) {
    return FtraceParseStatus::FTRACE_STATUS_SHORT_COMPACT_EVENT;
  }
  auto bundler = GetOrCreateBundler(sample.common.cpu);
  CpuReader::ParseSchedWakingCompact(
      sample.raw_data.data(), sample.common.timestamp, &sched_waking_format,
      bundler->CompactSchedBuf(), &metadata_);

  bundler->SetLastEventTimestamp(sample.common.timestamp);
  bundler->MaybeFinalize();
  return FtraceParseStatus::FTRACE_STATUS_OK;
}

void FtraceEventManager::Flush() {
  for (auto& cpu_bundler : cpu_bundler_map_) {
    auto bundler = cpu_bundler.second.get();
    bundler->Finalize(true /*close*/);
  }
  EmitAndClearParseErrors();
}

void FtraceEventManager::Flush(uint32_t cpu, bool events_lost) {
  auto it = cpu_bundler_map_.find(cpu);
  if (it != cpu_bundler_map_.end()) {
    auto bundler = it->second.get();
    bundler->SetLostEvents(events_lost);
    bundler->Finalize(false /*close*/);
  }
}

void FtraceEventManager::WriteAndSetParseError(
    FtraceEventManager::Bundler* bundler,
    uint64_t timestamp,
    FtraceParseStatus status) {
  PERFETTO_DLOG("FtraceEventManager: Error parsing event: %s",
                protos::pbzero::FtraceParseStatus_Name(status));

  parse_errors_.insert(status);
  auto* error = bundler->AddError();
  if (timestamp) {
    error->set_timestamp(timestamp);
  }
  error->set_status(status);
}

void FtraceEventManager::EmitAndClearParseErrors() {
  if (parse_errors_.empty()) {
    return;
  }
  auto packet = trace_writer_->NewTracePacket();
  auto ftrace_stats = packet->set_ftrace_stats();
  for (auto error : parse_errors_) {
    ftrace_stats->add_ftrace_parse_errors(error);
  }
  ftrace_stats->Finalize();
  parse_errors_.clear();
}

FtraceEventManager::Bundler::Bundler(
    std::unique_ptr<TraceWriter> trace_writer,
    uint32_t cpu,
    int32_t clock_id,
    FtraceMetadata* metadata,
    const GenericEventProtoDescriptors* generic_pb_descriptors,
    bool comp_sched_enabled)
    : trace_writer_(std::move(trace_writer)),
      ftrace_clock_(ToFtraceClock(clock_id)),
      cpu_bundler_(trace_writer_.get(),
                   metadata,
                   nullptr,
                   cpu,
                   std::nullopt,
                   &comp_sched_buf_,
                   comp_sched_enabled,
                   0,
                   generic_pb_descriptors) {}

}  // namespace profiling
}  // namespace perfetto
