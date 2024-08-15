/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/common/track_tracker.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto {
namespace trace_processor {

namespace {

const char* GetNameForGroup(TrackTracker::Group group) {
  switch (group) {
    case TrackTracker::Group::kMemory:
      return "Memory";
    case TrackTracker::Group::kIo:
      return "IO";
    case TrackTracker::Group::kVirtio:
      return "Virtio";
    case TrackTracker::Group::kNetwork:
      return "Network";
    case TrackTracker::Group::kPower:
      return "Power";
    case TrackTracker::Group::kDeviceState:
      return "Device State";
    case TrackTracker::Group::kThermals:
      return "Thermals";
    case TrackTracker::Group::kClockFrequency:
      return "Clock Freqeuncy";
    case TrackTracker::Group::kBatteryMitigation:
      return "Battery Mitigation";
    case TrackTracker::Group::kSizeSentinel:
      PERFETTO_FATAL("Unexpected size passed as group");
  }
  PERFETTO_FATAL("For GCC");
}

const char* GetUniqueTrackName(TrackTracker::UniqueTrackType type) {
  switch (type) {
    case TrackTracker::UniqueTrackType::kTrigger:
      return "Trace Triggers";
    case TrackTracker::UniqueTrackType::kInterconnect:
      return "Interconnect Events";
    case TrackTracker::UniqueTrackType::kChromeLegacyGlobalInstant:
      return "";
  }
  PERFETTO_FATAL("For gcc");
}

std::string GetCpuTrackName(TrackTracker::CpuTrackType type, uint32_t cpu) {
  switch (type) {
    case TrackTracker::CpuTrackType::kIrqCpu:
      return base::StackString<255>("Irq Cpu %u", cpu).c_str();
    case TrackTracker::CpuTrackType::kSortIrqCpu:
      return base::StackString<255>("SoftIrq Cpu %u", cpu).c_str();
    case TrackTracker::CpuTrackType::kNapiGroCpu:
      return base::StackString<255>("Napi Gro Cpu %u", cpu).c_str();
    case TrackTracker::CpuTrackType::kMaxFreqCpu:
      return base::StackString<255>("Cpu %u Max Freq Limit", cpu).c_str();
    case TrackTracker::CpuTrackType::kMinFreqCpu:
      return base::StackString<255>("Cpu %u Min Freq Limit", cpu).c_str();
    case TrackTracker::CpuTrackType::kFuncgraphCpu:
      return base::StackString<255>("swapper%u -funcgraph", cpu).c_str();
    case TrackTracker::CpuTrackType::kMaliIrqCpu:
      return base::StackString<255>("Mali Irq Cpu %u", cpu).c_str();
    case TrackTracker::CpuTrackType::kPkvmHypervisor:
      return base::StackString<255>("pkVM Hypervisor CPU %u", cpu).c_str();
  }
  PERFETTO_FATAL("For gcc");
}

std::string GetCpuCounterTrackName(TrackTracker::CpuCounterTrackType type,
                                   uint32_t cpu) {
  switch (type) {
    case TrackTracker::CpuCounterTrackType::kFrequency:
      return "cpufreq";
    case TrackTracker::CpuCounterTrackType::kFreqThrottle:
      return "cpufreq_throttle";
    case TrackTracker::CpuCounterTrackType::kIdle:
      return "cpuidle";
    case TrackTracker::CpuCounterTrackType::kUtilization:
      return base::StackString<255>("Cpu %u Util", cpu).c_str();
    case TrackTracker::CpuCounterTrackType::kCapacity:
      return base::StackString<255>("Cpu %u Cap", cpu).c_str();
    case TrackTracker::CpuCounterTrackType::kNrRunning:
      return base::StackString<255>("Cpu %u Nr Running", cpu).c_str();
    case TrackTracker::CpuCounterTrackType::kMaxFreqLimit:
      return base::StackString<255>("Cpu %u Max Freq Limit", cpu).c_str();
    case TrackTracker::CpuCounterTrackType::kMinFreqLimit:
      return base::StackString<255>("Cpu %u Min Freq Limit", cpu).c_str();
    case TrackTracker::CpuCounterTrackType::kUserTime:
      return "cpu.times.user_ns";
    case TrackTracker::CpuCounterTrackType::kNiceUserTime:
      return "cpu.times.user_nice_ns";
    case TrackTracker::CpuCounterTrackType::kSystemModeTime:
      return "cpu.times.system_mode_ns";
    case TrackTracker::CpuCounterTrackType::kIdleTime:
      return "cpu.times.idle_ns";
    case TrackTracker::CpuCounterTrackType::kIoWaitTime:
      return "cpu.times.io_wait_ns";
    case TrackTracker::CpuCounterTrackType::kIrqTime:
      return "cpu.times.irq_ns";
    case TrackTracker::CpuCounterTrackType::kSoftIrqTime:
      return "cpu.times.softirq_ns";
    case TrackTracker::CpuCounterTrackType::kIdleState:
      PERFETTO_FATAL("Idle state CPU counter doesn't have standard name");
  }
  PERFETTO_FATAL("For gcc");
}

const char* GetGpuCounterTrackName(TrackTracker::GpuCounterTrackType type) {
  switch (type) {
    case TrackTracker::GpuCounterTrackType::kFreqency:
      return "gpufreq";
  }
  PERFETTO_FATAL("For gcc");
}

const char* GetIrqCounterTrackName(TrackTracker::IrqCounterTrackType type) {
  switch (type) {
    case TrackTracker::IrqCounterTrackType::kIrqCount:
      return "num_irq";
  }
  PERFETTO_FATAL("For gcc");
}

const char* GetSoftIrqCounterTrackName(
    TrackTracker::SoftIrqCounterTrackType type) {
  switch (type) {
    case TrackTracker::SoftIrqCounterTrackType::kSoftIrqCount:
      return "num_softirq";
  }
  PERFETTO_FATAL("For gcc");
}

}  // namespace

TrackTracker::TrackTracker(TraceProcessorContext* context)
    : source_key_(context->storage->InternString("source")),
      trace_id_key_(context->storage->InternString("trace_id")),
      trace_id_is_process_scoped_key_(
          context->storage->InternString("trace_id_is_process_scoped")),
      source_scope_key_(context->storage->InternString("source_scope")),
      category_key_(context->storage->InternString("category")),
      fuchsia_source_(context->storage->InternString("fuchsia")),
      chrome_source_(context->storage->InternString("chrome")),
      context_(context) {}

TrackId TrackTracker::InternThreadTrack(UniqueTid utid) {
  auto it = thread_tracks_.find(utid);
  if (it != thread_tracks_.end())
    return it->second;

  tables::ThreadTrackTable::Row row;
  row.utid = utid;
  row.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_thread_track_table()->Insert(row).id;
  thread_tracks_[utid] = id;
  return id;
}

TrackId TrackTracker::InternProcessTrack(UniquePid upid) {
  auto it = process_tracks_.find(upid);
  if (it != process_tracks_.end())
    return it->second;

  tables::ProcessTrackTable::Row row;
  row.upid = upid;
  row.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_process_track_table()->Insert(row).id;
  process_tracks_[upid] = id;
  return id;
}

TrackId TrackTracker::InternCpuTrack(CpuTrackType type, uint32_t cpu) {
  std::string track_name = GetCpuTrackName(type, cpu);
  StringId name = context_->storage->InternString(track_name.c_str());
  auto it = cpu_tracks_.find(std::make_pair(name, cpu));
  if (it != cpu_tracks_.end()) {
    return it->second;
  }

  tables::CpuTrackTable::Row row(name);
  row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  row.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_cpu_track_table()->Insert(row).id;
  cpu_tracks_[std::make_pair(name, cpu)] = id;
  return id;
}

TrackId TrackTracker::InternUniqueTrack(UniqueTrackType type) {
  auto it = unique_tracks_.find(type);
  if (it != unique_tracks_.end())
    return it->second;

  tables::TrackTable::Row row;
  row.name = context_->storage->InternString(GetUniqueTrackName(type));
  row.machine_id = context_->machine_id();
  TrackId id = context_->storage->mutable_track_table()->Insert(row).id;
  unique_tracks_[type] = id;

  if (type == UniqueTrackType::kChromeLegacyGlobalInstant) {
    context_->args_tracker->AddArgsTo(id).AddArg(
        source_key_, Variadic::String(chrome_source_));
  }

  return id;
}

TrackId TrackTracker::InternFuchsiaAsyncTrack(StringId name,
                                              uint32_t upid,
                                              int64_t correlation_id) {
  return InternLegacyChromeAsyncTrack(name, upid, correlation_id, false,
                                      StringId());
}

TrackId TrackTracker::InternGpuTrack(const tables::GpuTrackTable::Row& row) {
  GpuTrackTuple tuple{row.name, row.scope, row.context_id.value_or(0)};

  auto it = gpu_tracks_.find(tuple);
  if (it != gpu_tracks_.end())
    return it->second;

  auto row_copy = row;
  row_copy.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_gpu_track_table()->Insert(row_copy).id;
  gpu_tracks_[tuple] = id;
  return id;
}

TrackId TrackTracker::InternGpuWorkPeriodTrack(
    const tables::GpuWorkPeriodTrackTable::Row& row) {
  GpuWorkPeriodTrackTuple tuple{row.name, row.gpu_id, row.uid};

  auto it = gpu_work_period_tracks_.find(tuple);
  if (it != gpu_work_period_tracks_.end())
    return it->second;

  auto id =
      context_->storage->mutable_gpu_work_period_track_table()->Insert(row).id;
  gpu_work_period_tracks_[tuple] = id;
  return id;
}

TrackId TrackTracker::InternLegacyChromeAsyncTrack(
    StringId raw_name,
    uint32_t upid,
    int64_t trace_id,
    bool trace_id_is_process_scoped,
    StringId source_scope) {
  ChromeTrackTuple tuple;
  if (trace_id_is_process_scoped)
    tuple.upid = upid;
  tuple.trace_id = trace_id;
  tuple.source_scope = source_scope;

  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);
  auto it = chrome_tracks_.find(tuple);
  if (it != chrome_tracks_.end()) {
    if (name != kNullStringId) {
      // The track may have been created for an end event without name. In that
      // case, update it with this event's name.
      auto& tracks = *context_->storage->mutable_track_table();
      auto rr = *tracks.FindById(it->second);
      if (rr.name() == kNullStringId) {
        rr.set_name(name);
      }
    }
    return it->second;
  }

  // Legacy async tracks are always drawn in the context of a process, even if
  // the ID's scope is global.
  tables::ProcessTrackTable::Row track(name);
  track.upid = upid;
  track.machine_id = context_->machine_id();
  TrackId id =
      context_->storage->mutable_process_track_table()->Insert(track).id;
  chrome_tracks_[tuple] = id;

  context_->args_tracker->AddArgsTo(id)
      .AddArg(source_key_, Variadic::String(chrome_source_))
      .AddArg(trace_id_key_, Variadic::Integer(trace_id))
      .AddArg(trace_id_is_process_scoped_key_,
              Variadic::Boolean(trace_id_is_process_scoped))
      .AddArg(source_scope_key_, Variadic::String(source_scope));

  return id;
}

TrackId TrackTracker::CreateGlobalAsyncTrack(StringId name, StringId source) {
  tables::TrackTable::Row row(name);
  row.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_track_table()->Insert(row).id;
  if (!source.is_null()) {
    context_->args_tracker->AddArgsTo(id).AddArg(source_key_,
                                                 Variadic::String(source));
  }
  return id;
}

TrackId TrackTracker::CreateProcessAsyncTrack(StringId raw_name,
                                              UniquePid upid,
                                              StringId source) {
  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);
  tables::ProcessTrackTable::Row row(name);
  row.upid = upid;
  row.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_process_track_table()->Insert(row).id;
  if (!source.is_null()) {
    context_->args_tracker->AddArgsTo(id).AddArg(source_key_,
                                                 Variadic::String(source));
  }
  return id;
}

TrackId TrackTracker::InternLegacyChromeProcessInstantTrack(UniquePid upid) {
  auto it = chrome_process_instant_tracks_.find(upid);
  if (it != chrome_process_instant_tracks_.end())
    return it->second;

  tables::ProcessTrackTable::Row row;
  row.upid = upid;
  row.machine_id = context_->machine_id();
  auto id = context_->storage->mutable_process_track_table()->Insert(row).id;
  chrome_process_instant_tracks_[upid] = id;

  context_->args_tracker->AddArgsTo(id).AddArg(
      source_key_, Variadic::String(chrome_source_));

  return id;
}

TrackId TrackTracker::InternGlobalCounterTrack(TrackTracker::Group group,
                                               StringId name,
                                               SetArgsCallback callback,
                                               StringId unit,
                                               StringId description) {
  auto it = global_counter_tracks_by_name_.find(name);
  if (it != global_counter_tracks_by_name_.end()) {
    return it->second;
  }

  tables::CounterTrackTable::Row row(name);
  row.parent_id = InternTrackForGroup(group);
  row.unit = unit;
  row.description = description;
  row.machine_id = context_->machine_id();
  TrackId track =
      context_->storage->mutable_counter_track_table()->Insert(row).id;
  global_counter_tracks_by_name_[name] = track;
  if (callback) {
    auto inserter = context_->args_tracker->AddArgsTo(track);
    callback(inserter);
  }
  return track;
}

TrackId TrackTracker::InternCpuCounterTrack(CpuCounterTrackTuple tuple) {
  StringPool::Id name =
      tuple.name.is_null()
          ? context_->storage->InternString(
                GetCpuCounterTrackName(tuple.type, tuple.cpu).c_str())
          : tuple.name;
  auto it = cpu_counter_tracks_.find(tuple);
  if (it != cpu_counter_tracks_.end()) {
    return it->second;
  }

  tables::CpuCounterTrackTable::Row row(name);
  row.ucpu = context_->cpu_tracker->GetOrCreateCpu(tuple.cpu);
  row.machine_id = context_->machine_id();

  TrackId track =
      context_->storage->mutable_cpu_counter_track_table()->Insert(row).id;
  cpu_counter_tracks_[tuple] = track;
  return track;
}

TrackId TrackTracker::InternCpuCounterTrack(CpuCounterTrackType track_type,
                                            uint32_t cpu) {
  CpuCounterTrackTuple tuple{track_type, cpu};
  return InternCpuCounterTrack(tuple);
}

TrackId TrackTracker::InternCpuIdleStateTrack(uint32_t cpu, StringId state) {
  std::string name =
      "cpuidle." + context_->storage->GetString(state).ToStdString();

  CpuCounterTrackTuple tuple{TrackTracker::CpuCounterTrackType::kIdleState, cpu,
                             context_->storage->InternString(name.c_str()),
                             state.raw_id()};
  return InternCpuCounterTrack(tuple);
}

TrackId TrackTracker::InternThreadCounterTrack(StringId name, UniqueTid utid) {
  auto it = utid_counter_tracks_.find(std::make_pair(name, utid));
  if (it != utid_counter_tracks_.end()) {
    return it->second;
  }

  tables::ThreadCounterTrackTable::Row row(name);
  row.utid = utid;
  row.machine_id = context_->machine_id();

  TrackId track =
      context_->storage->mutable_thread_counter_track_table()->Insert(row).id;
  utid_counter_tracks_[std::make_pair(name, utid)] = track;
  return track;
}

TrackId TrackTracker::InternProcessCounterTrack(StringId raw_name,
                                                UniquePid upid,
                                                StringId unit,
                                                StringId description) {
  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);
  auto it = upid_counter_tracks_.find(std::make_pair(name, upid));
  if (it != upid_counter_tracks_.end()) {
    return it->second;
  }

  tables::ProcessCounterTrackTable::Row row(name);
  row.upid = upid;
  row.unit = unit;
  row.description = description;
  row.machine_id = context_->machine_id();

  TrackId track =
      context_->storage->mutable_process_counter_track_table()->Insert(row).id;
  upid_counter_tracks_[std::make_pair(name, upid)] = track;
  return track;
}

TrackId TrackTracker::InternIrqCounterTrack(IrqCounterTrackType type,
                                            int32_t irq) {
  auto it = irq_counter_tracks_.find(std::make_pair(type, irq));
  if (it != irq_counter_tracks_.end()) {
    return it->second;
  }

  tables::IrqCounterTrackTable::Row row(
      context_->storage->InternString(GetIrqCounterTrackName(type)));
  row.irq = irq;
  row.machine_id = context_->machine_id();

  TrackId track =
      context_->storage->mutable_irq_counter_track_table()->Insert(row).id;
  irq_counter_tracks_[std::make_pair(type, irq)] = track;
  return track;
}

TrackId TrackTracker::InternSoftirqCounterTrack(SoftIrqCounterTrackType type,
                                                int32_t softirq) {
  auto it = softirq_counter_tracks_.find(std::make_pair(type, softirq));
  if (it != softirq_counter_tracks_.end()) {
    return it->second;
  }

  tables::SoftirqCounterTrackTable::Row row(
      context_->storage->InternString(GetSoftIrqCounterTrackName(type)));
  row.softirq = softirq;
  row.machine_id = context_->machine_id();

  TrackId track =
      context_->storage->mutable_softirq_counter_track_table()->Insert(row).id;
  softirq_counter_tracks_[std::make_pair(type, softirq)] = track;
  return track;
}

TrackId TrackTracker::InternGpuCounterTrack(GpuCounterTrackType type,
                                            uint32_t gpu_id) {
  StringId name = context_->storage->InternString(GetGpuCounterTrackName(type));
  auto it = gpu_counter_tracks_.find(std::make_pair(type, gpu_id));
  if (it != gpu_counter_tracks_.end()) {
    return it->second;
  }
  TrackId track = CreateGpuCounterTrack(name, gpu_id);
  gpu_counter_tracks_[std::make_pair(type, gpu_id)] = track;
  return track;
}

TrackId TrackTracker::InternEnergyCounterTrack(StringId name,
                                               int32_t consumer_id,
                                               StringId consumer_type,
                                               int32_t ordinal) {
  auto it = energy_counter_tracks_.find(std::make_pair(name, consumer_id));
  if (it != energy_counter_tracks_.end()) {
    return it->second;
  }
  tables::EnergyCounterTrackTable::Row row(name);
  row.consumer_id = consumer_id;
  row.consumer_type = consumer_type;
  row.ordinal = ordinal;
  row.machine_id = context_->machine_id();
  TrackId track =
      context_->storage->mutable_energy_counter_track_table()->Insert(row).id;
  energy_counter_tracks_[std::make_pair(name, consumer_id)] = track;
  return track;
}

TrackId TrackTracker::InternEnergyPerUidCounterTrack(StringId name,
                                                     int32_t consumer_id,
                                                     int32_t uid) {
  auto it = energy_per_uid_counter_tracks_.find(std::make_pair(name, uid));
  if (it != energy_per_uid_counter_tracks_.end()) {
    return it->second;
  }

  tables::EnergyPerUidCounterTrackTable::Row row(name);
  row.consumer_id = consumer_id;
  row.uid = uid;
  row.machine_id = context_->machine_id();
  TrackId track =
      context_->storage->mutable_energy_per_uid_counter_track_table()
          ->Insert(row)
          .id;
  energy_per_uid_counter_tracks_[std::make_pair(name, uid)] = track;
  return track;
}

TrackId TrackTracker::InternLinuxDeviceTrack(StringId name) {
  if (auto it = linux_device_tracks_.find(name);
      it != linux_device_tracks_.end()) {
    return it->second;
  }

  tables::LinuxDeviceTrackTable::Row row(name);
  TrackId track =
      context_->storage->mutable_linux_device_track_table()->Insert(row).id;
  linux_device_tracks_[name] = track;
  return track;
}

TrackId TrackTracker::CreateGpuCounterTrack(StringId name,
                                            uint32_t gpu_id,
                                            StringId description,
                                            StringId unit) {
  tables::GpuCounterTrackTable::Row row(name);
  row.gpu_id = gpu_id;
  row.description = description;
  row.unit = unit;
  row.machine_id = context_->machine_id();

  return context_->storage->mutable_gpu_counter_track_table()->Insert(row).id;
}

TrackId TrackTracker::CreatePerfCounterTrack(
    StringId name,
    tables::PerfSessionTable::Id perf_session_id,
    uint32_t cpu,
    bool is_timebase) {
  tables::PerfCounterTrackTable::Row row(name);
  row.perf_session_id = perf_session_id;
  row.cpu = cpu;
  row.is_timebase = is_timebase;
  row.machine_id = context_->machine_id();
  return context_->storage->mutable_perf_counter_track_table()->Insert(row).id;
}

TrackId TrackTracker::InternTrackForGroup(TrackTracker::Group group) {
  uint32_t group_idx = static_cast<uint32_t>(group);
  const std::optional<TrackId>& group_id = group_track_ids_[group_idx];
  if (group_id) {
    return *group_id;
  }

  StringId id = context_->storage->InternString(GetNameForGroup(group));
  tables::TrackTable::Row row{id};
  row.machine_id = context_->machine_id();
  TrackId track_id = context_->storage->mutable_track_table()->Insert(row).id;
  group_track_ids_[group_idx] = track_id;
  return track_id;
}

}  // namespace trace_processor
}  // namespace perfetto
