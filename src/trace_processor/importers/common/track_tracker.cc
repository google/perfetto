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

#include "perfetto/ext/base/string_utils.h"
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

// This function is added to keep backward compatibility. Don't add new names.
inline std::optional<std::string> GetCpuTrackName(
    TrackTracker::TrackClassification type,
    uint32_t cpu) {
  std::optional<std::string> track_name;
  if (type == TrackTracker::TrackClassification::kIrqCpu)
    track_name = base::StackString<255>("Irq Cpu %u", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kSoftirqCpu)
    track_name = base::StackString<255>("SoftIrq Cpu %u", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kNapiGroCpu)
    track_name = base::StackString<255>("Napi Gro Cpu %u", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kMaxFreqCpu)
    track_name = base::StackString<255>("Cpu %u Max Freq Limit", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kMinFreqCpu)
    track_name = base::StackString<255>("Cpu %u Min Freq Limit", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kFuncgraphCpu)
    track_name = base::StackString<255>("swapper%u -funcgraph", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kMaliIrqCpu)
    track_name = base::StackString<255>("Mali Irq Cpu %u", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kPkvmHypervisor)
    track_name = base::StackString<255>("pkVM Hypervisor CPU %u", cpu).c_str();
  return track_name;
}

// This function is added to keep backward compatibility. Don't add new names.
inline std::optional<std::string> GetCpuCounterTrackName(
    TrackTracker::TrackClassification type,
    uint32_t cpu) {
  std::optional<std::string> maybe_name;
  if (type == TrackTracker::TrackClassification::kCpuFrequency)
    maybe_name = "cpufreq";
  else if (type == TrackTracker::TrackClassification::kCpuFrequencyThrottle)
    maybe_name = "cpufreq_throttle";
  else if (type == TrackTracker::TrackClassification::kCpuIdle)
    maybe_name = "cpuidle";
  else if (type == TrackTracker::TrackClassification::kUserTime)
    maybe_name = "cpu.times.user_ns";
  else if (type == TrackTracker::TrackClassification::kNiceUserTime)
    maybe_name = "cpu.times.user_nice_ns";
  else if (type == TrackTracker::TrackClassification::kSystemModeTime)
    maybe_name = "cpu.times.system_mode_ns";
  else if (type == TrackTracker::TrackClassification::kCpuIdleTime)
    maybe_name = "cpu.times.idle_ns";
  else if (type == TrackTracker::TrackClassification::kIoWaitTime)
    maybe_name = "cpu.times.io_wait_ns";
  else if (type == TrackTracker::TrackClassification::kIrqTime)
    maybe_name = "cpu.times.irq_ns";
  else if (type == TrackTracker::TrackClassification::kSoftIrqTime)
    maybe_name = "cpu.times.softirq_ns";
  else if (type == TrackTracker::TrackClassification::kCpuUtilization)
    maybe_name = base::StackString<255>("Cpu %u Util", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kCpuCapacity)
    maybe_name = base::StackString<255>("Cpu %u Cap", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kCpuNumberRunning)
    maybe_name = base::StackString<255>("Cpu %u Nr Running", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kCpuMaxFrequencyLimit)
    maybe_name = base::StackString<255>("Cpu %u Max Freq Limit", cpu).c_str();
  else if (type == TrackTracker::TrackClassification::kCpuMinFrequencyLimit)
    maybe_name = base::StackString<255>("Cpu %u Min Freq Limit", cpu).c_str();
  return maybe_name;
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
      utid_id_(context->storage->InternString("utid")),
      upid_id_(context->storage->InternString("upid")),
      ucpu_id_(context->storage->InternString("ucpu")),
      uid_id_(context->storage->InternString("uid")),
      gpu_id_(context->storage->InternString("gpu")),
      name_id_(context->storage->InternString("name")),
      context_(context) {}

TrackId TrackTracker::InternTrack(TrackClassification classification,
                                  std::optional<Dimensions> dimensions,
                                  StringId name) {
  auto it = tracks_.Find({classification, dimensions});
  if (it)
    return *it;

  tables::TrackTable::Row row;
  row.classification = context_->storage->InternString(
      std::string(GetClassification(classification)).c_str());
  if (dimensions) {
    row.dimensions = dimensions->arg_set_id;
  }
  row.machine_id = context_->machine_id();
  row.name = name;

  TrackId id = context_->storage->mutable_track_table()->Insert(row).id;
  tracks_[{classification, dimensions}] = id;
  return id;
}

TrackId TrackTracker::CreateAsyncTrack(std::optional<Dimensions> dimensions,
                                       StringId name) {
  tables::TrackTable::Row row;
  row.classification = context_->storage->InternString(
      std::string(GetClassification(TrackClassification::kAsync)).c_str());
  if (dimensions) {
    row.dimensions = dimensions->arg_set_id;
  }
  row.machine_id = context_->machine_id();
  row.name = name;

  TrackId id = context_->storage->mutable_track_table()->Insert(row).id;
  tracks_[{TrackTracker::TrackClassification::kAsync, dimensions}] = id;
  return id;
}

TrackId TrackTracker::InternProcessTrack(
    TrackClassification classification,
    UniquePid upid,
    std::optional<Dimensions> use_other_dimension,
    StringId name) {
  Dimensions dims_id = use_other_dimension.has_value()
                           ? use_other_dimension.value()
                           : SingleDimension(upid_id_, Variadic::Integer(upid));
  auto it = tracks_.Find({classification, dims_id});
  if (it)
    return *it;

  tables::ProcessTrackTable::Row row(name);
  row.upid = upid;
  row.dimensions = dims_id.arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(classification).c_str());
  row.machine_id = context_->machine_id();

  auto track_id =
      context_->storage->mutable_process_track_table()->Insert(row).id;
  tracks_[{classification, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::InternThreadTrack(UniqueTid utid) {
  Dimensions dims_id = SingleDimension(utid_id_, Variadic::Integer(utid));

  auto it = tracks_.Find({TrackClassification::kThread, dims_id});
  if (it)
    return *it;

  tables::ThreadTrackTable::Row row;
  row.utid = utid;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kThread).c_str());
  row.dimensions = dims_id.arg_set_id;
  row.machine_id = context_->machine_id();

  auto track_id =
      context_->storage->mutable_thread_track_table()->Insert(row).id;
  tracks_[{TrackClassification::kThread, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::InternProcessTrack(UniquePid upid) {
  Dimensions dims_id = SingleDimension(upid_id_, Variadic::Integer(upid));
  auto it = tracks_.Find({TrackClassification::kProcess, dims_id});
  if (it)
    return *it;

  tables::ProcessTrackTable::Row row;
  row.upid = upid;
  row.dimensions = dims_id.arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kProcess).c_str());
  row.machine_id = context_->machine_id();

  auto track_id =
      context_->storage->mutable_process_track_table()->Insert(row).id;
  tracks_[{TrackClassification::kProcess, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::InternCpuTrack(TrackClassification type, uint32_t cpu) {
  auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  Dimensions dims_id = SingleDimension(ucpu_id_, Variadic::Integer(ucpu.value));

  auto it = tracks_.Find({type, dims_id});
  if (it) {
    return *it;
  }

  tables::CpuTrackTable::Row row;
  row.ucpu = ucpu;
  row.machine_id = context_->machine_id();
  row.classification =
      context_->storage->InternString(GetClassification(type).c_str());
  row.dimensions = dims_id.arg_set_id;
  if (std::optional<std::string> track_name = GetCpuTrackName(type, cpu)) {
    row.name = context_->storage->InternString(track_name.value().c_str());
  }

  auto id = context_->storage->mutable_cpu_track_table()->Insert(row).id;

  tracks_[{type, dims_id}] = id;
  return id;
}

TrackId TrackTracker::InternGlobalTrack(TrackClassification type) {
  auto it = tracks_.Find({type, std::nullopt});
  if (it)
    return *it;

  StringId name;
  if (type == TrackTracker::TrackClassification::kTrigger)
    name = context_->storage->InternString("Trace Triggers");
  if (type == TrackTracker::TrackClassification::kInterconnect)
    name = context_->storage->InternString("Interconnect Events");

  TrackId track_id = InternTrack(type, std::nullopt, name);
  tracks_[{type, std::nullopt}] = track_id;

  if (type == TrackClassification::kChromeLegacyGlobalInstant) {
    context_->args_tracker->AddArgsTo(track_id).AddArg(
        source_key_, Variadic::String(chrome_source_));
  }

  return track_id;
}

TrackId TrackTracker::LegacyInternFuchsiaAsyncTrack(StringId name,
                                                    uint32_t upid,
                                                    int64_t correlation_id) {
  return LegacyInternLegacyChromeAsyncTrack(name, upid, correlation_id, false,
                                            StringId());
}

TrackId TrackTracker::InternGpuTrack(const tables::GpuTrackTable::Row& row) {
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendGpu(row.context_id.value_or(0));
  if (row.scope != kNullStringId) {
    dims_builder.AppendDimension(context_->storage->InternString("scope"),
                                 Variadic::String(row.scope));
  }
  Dimensions dims_id = std::move(dims_builder).Build();

  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.dimensions = dims_id;
  key.name = row.name;

  auto it = tracks_.Find(key);
  if (it)
    return *it;

  auto row_copy = row;
  row_copy.classification = context_->storage->InternString(
      std::string(GetClassification(TrackClassification::kUnknown)).c_str());
  row_copy.dimensions = dims_id.arg_set_id;
  row_copy.machine_id = context_->machine_id();

  auto id = context_->storage->mutable_gpu_track_table()->Insert(row_copy).id;
  tracks_[key] = id;
  return id;
}

TrackId TrackTracker::LegacyInternGpuWorkPeriodTrack(
    const tables::GpuWorkPeriodTrackTable::Row& row) {
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendGpu(row.gpu_id);
  dims_builder.AppendUid(row.uid);
  Dimensions dims_id = std::move(dims_builder).Build();

  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.dimensions = dims_id;
  key.name = row.name;

  auto it = tracks_.Find(key);
  if (it)
    return *it;

  tables::GpuWorkPeriodTrackTable::Row row_copy = row;
  row_copy.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());
  row_copy.dimensions = dims_id.arg_set_id;
  row_copy.machine_id = context_->machine_id();

  auto id = context_->storage->mutable_gpu_work_period_track_table()
                ->Insert(row_copy)
                .id;
  tracks_[key] = id;
  return id;
}

TrackId TrackTracker::LegacyInternLegacyChromeAsyncTrack(
    StringId raw_name,
    uint32_t upid,
    int64_t trace_id,
    bool trace_id_is_process_scoped,
    StringId source_scope) {
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendDimension(context_->storage->InternString("scope"),
                               Variadic::String(source_scope));
  if (trace_id_is_process_scoped) {
    dims_builder.AppendUpid(upid);
  }
  Dimensions dims_id = std::move(dims_builder).Build();

  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);

  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.cookie = trace_id;
  key.dimensions = dims_id;

  auto it = tracks_.Find(key);
  if (it) {
    if (name != kNullStringId) {
      // The track may have been created for an end event without name. In
      // that case, update it with this event's name.
      auto& tracks = *context_->storage->mutable_track_table();
      auto rr = *tracks.FindById(*it);
      if (rr.name() == kNullStringId) {
        rr.set_name(name);
      }
    }
    return *it;
  }

  // Legacy async tracks are always drawn in the context of a process, even if
  // the ID's scope is global.
  tables::ProcessTrackTable::Row track(name);
  track.upid = upid;
  track.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());
  track.dimensions = dims_id.arg_set_id;
  track.machine_id = context_->machine_id();

  TrackId id =
      context_->storage->mutable_process_track_table()->Insert(track).id;
  tracks_[key] = id;

  context_->args_tracker->AddArgsTo(id)
      .AddArg(source_key_, Variadic::String(chrome_source_))
      .AddArg(trace_id_key_, Variadic::Integer(trace_id))
      .AddArg(trace_id_is_process_scoped_key_,
              Variadic::Boolean(trace_id_is_process_scoped))
      .AddArg(source_scope_key_, Variadic::String(source_scope));

  return id;
}

TrackId TrackTracker::LegacyCreateGlobalAsyncTrack(StringId name,
                                                   StringId source) {
  Dimensions dims_id = SingleDimension(name_id_, Variadic::String(name));
  TrackId track_id = CreateAsyncTrack(dims_id, name);
  if (!source.is_null()) {
    context_->args_tracker->AddArgsTo(track_id).AddArg(
        source_key_, Variadic::String(source));
  }
  return track_id;
}

TrackId TrackTracker::LegacyCreateProcessAsyncTrack(StringId raw_name,
                                                    UniquePid upid,
                                                    StringId source) {
  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendName(name);
  dims_builder.AppendUpid(upid);
  Dimensions dims_id = std::move(dims_builder).Build();

  tables::ProcessTrackTable::Row row(name);
  row.upid = upid;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kAsync).c_str());
  row.machine_id = context_->machine_id();
  row.dimensions = dims_id.arg_set_id;

  auto id = context_->storage->mutable_process_track_table()->Insert(row).id;
  if (!source.is_null()) {
    context_->args_tracker->AddArgsTo(id).AddArg(source_key_,
                                                 Variadic::String(source));
  }
  return id;
}

TrackId TrackTracker::InternLegacyChromeProcessInstantTrack(UniquePid upid) {
  TrackId track_id = InternProcessTrack(
      TrackTracker::TrackClassification::kChromeProcessInstant, upid);
  context_->args_tracker->AddArgsTo(track_id).AddArg(
      source_key_, Variadic::String(chrome_source_));

  return track_id;
}

TrackId TrackTracker::InternGlobalCounterTrack(TrackTracker::Group group,
                                               StringId name,
                                               SetArgsCallback callback,
                                               StringId unit,
                                               StringId description) {
  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.name = name;

  auto it = tracks_.Find(key);
  if (it) {
    return *it;
  }

  tables::CounterTrackTable::Row row(name);
  row.parent_id = InternTrackForGroup(group);
  row.unit = unit;
  row.description = description;
  row.machine_id = context_->machine_id();
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());

  TrackId track =
      context_->storage->mutable_counter_track_table()->Insert(row).id;
  tracks_[key] = track;

  if (callback) {
    auto inserter = context_->args_tracker->AddArgsTo(track);
    callback(inserter);
  }

  return track;
}

TrackId TrackTracker::InternCpuCounterTrack(TrackClassification type,
                                            uint32_t cpu) {
  auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  Dimensions dims_id = SingleDimension(ucpu_id_, Variadic::Integer(ucpu.value));

  TrackMapKey key;
  key.classification = type;
  key.dimensions = dims_id;
  if (std::optional<std::string> maybe_name =
          GetCpuCounterTrackName(type, cpu)) {
    key.name = std::make_optional(
        context_->storage->InternString(maybe_name->c_str()));
  }

  auto it = tracks_.Find(key);
  if (it) {
    return *it;
  }

  tables::CpuCounterTrackTable::Row row;
  row.ucpu = ucpu;
  row.machine_id = context_->machine_id();
  row.classification =
      context_->storage->InternString(GetClassification(type).c_str());
  row.dimensions = dims_id.arg_set_id;
  if (key.name) {
    row.name = *key.name;
  }

  TrackId track_id =
      context_->storage->mutable_cpu_counter_track_table()->Insert(row).id;

  tracks_[key] = track_id;
  return track_id;
}

TrackId TrackTracker::LegacyInternCpuIdleStateTrack(uint32_t cpu,
                                                    StringId state) {
  auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendDimension(
      context_->storage->InternString("cpu_idle_state"),
      Variadic::String(state));
  dims_builder.AppendCpu(ucpu.value);
  Dimensions dims_id = std::move(dims_builder).Build();

  TrackClassification classification = TrackClassification::kCpuIdleState;

  auto it = tracks_.Find({classification, dims_id});
  if (it) {
    return *it;
  }

  std::string name =
      "cpuidle." + context_->storage->GetString(state).ToStdString();

  tables::CpuCounterTrackTable::Row row(
      context_->storage->InternString(name.c_str()));
  row.ucpu = ucpu;
  row.machine_id = context_->machine_id();
  row.classification = context_->storage->InternString(
      GetClassification(classification).c_str());
  row.dimensions = dims_id.arg_set_id;

  TrackId track_id =
      context_->storage->mutable_cpu_counter_track_table()->Insert(row).id;

  tracks_[{classification, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::InternThreadCounterTrack(StringId name, UniqueTid utid) {
  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.name = name;
  key.dimensions = SingleDimension(utid_id_, Variadic::Integer(utid));

  auto it = tracks_.Find(key);
  if (it) {
    return *it;
  }

  tables::ThreadCounterTrackTable::Row row(name);
  row.utid = utid;
  row.machine_id = context_->machine_id();
  row.dimensions = key.dimensions->arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(key.classification).c_str());

  TrackId track_id =
      context_->storage->mutable_thread_counter_track_table()->Insert(row).id;

  tracks_[key] = track_id;
  return track_id;
}

TrackId TrackTracker::InternProcessCounterTrack(StringId raw_name,
                                                UniquePid upid,
                                                StringId unit,
                                                StringId description) {
  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);

  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.name = name;
  key.dimensions = SingleDimension(upid_id_, Variadic::Integer(upid));

  auto it = tracks_.Find(key);
  if (it) {
    return *it;
  }

  tables::ProcessCounterTrackTable::Row row(name);
  row.upid = upid;
  row.unit = unit;
  row.description = description;
  row.machine_id = context_->machine_id();
  row.classification = context_->storage->InternString(
      GetClassification(key.classification).c_str());
  row.dimensions = key.dimensions->arg_set_id;

  TrackId track_id =
      context_->storage->mutable_process_counter_track_table()->Insert(row).id;

  tracks_[key] = track_id;
  return track_id;
}

TrackId TrackTracker::LegacyInternIrqCounterTrack(TrackClassification type,
                                                  int32_t irq) {
  Dimensions dims_id = SingleDimension(context_->storage->InternString("irq"),
                                       Variadic::Integer(irq));

  auto it = tracks_.Find({type, dims_id});
  if (it) {
    return *it;
  }

  tables::IrqCounterTrackTable::Row row;
  row.irq = irq;
  row.machine_id = context_->machine_id();
  row.classification =
      context_->storage->InternString(GetClassification(type).c_str());
  row.dimensions = dims_id.arg_set_id;

  if (type == TrackTracker::TrackClassification::kIrqCount)
    row.name = context_->storage->InternString("num_irq");

  TrackId track_id =
      context_->storage->mutable_irq_counter_track_table()->Insert(row).id;

  tracks_[{type, dims_id}] = track_id;

  return track_id;
}

TrackId TrackTracker::LegacyInternSoftirqCounterTrack(TrackClassification type,
                                                      int32_t softirq) {
  Dimensions dims_id = SingleDimension(
      context_->storage->InternString("softirq"), Variadic::Integer(softirq));
  auto it = tracks_.Find({type, dims_id});
  if (it) {
    return *it;
  }

  tables::SoftirqCounterTrackTable::Row row;
  row.softirq = softirq;
  row.machine_id = context_->machine_id();
  row.classification =
      context_->storage->InternString(GetClassification(type).c_str());
  row.dimensions = dims_id.arg_set_id;

  if (type == TrackTracker::TrackClassification::kSoftirqCount)
    row.name = context_->storage->InternString("num_softirq");

  TrackId track_id =
      context_->storage->mutable_softirq_counter_track_table()->Insert(row).id;
  tracks_[{type, dims_id}] = track_id;

  return track_id;
}

TrackId TrackTracker::InternGpuCounterTrack(TrackClassification type,
                                            uint32_t gpu_id) {
  Dimensions dims_id = SingleDimension(gpu_id_, Variadic::Integer(gpu_id));

  auto it = tracks_.Find({type, dims_id});
  if (it) {
    return *it;
  }

  tables::GpuCounterTrackTable::Row row;
  row.gpu_id = gpu_id;
  row.machine_id = context_->machine_id();
  row.dimensions = dims_id.arg_set_id;
  row.classification =
      context_->storage->InternString(GetClassification(type).c_str());
  if (type == TrackTracker::TrackClassification::kGpuFrequency)
    row.name = context_->storage->InternString("gpufreq");

  TrackId track_id =
      context_->storage->mutable_gpu_counter_track_table()->Insert(row).id;

  tracks_[{type, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::LegacyInternLegacyEnergyCounterTrack(
    StringId name,
    int32_t consumer_id,
    StringId consumer_type,
    int32_t ordinal) {
  Dimensions dims_id =
      SingleDimension(context_->storage->InternString("energy_consumer_id"),
                      Variadic::Integer(consumer_id));

  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.dimensions = dims_id;
  key.name = name;

  auto it = tracks_.Find(key);
  if (it) {
    return *it;
  }
  tables::EnergyCounterTrackTable::Row row(name);
  row.consumer_id = consumer_id;
  row.consumer_type = consumer_type;
  row.ordinal = ordinal;
  row.machine_id = context_->machine_id();
  row.dimensions = dims_id.arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());

  TrackId track =
      context_->storage->mutable_energy_counter_track_table()->Insert(row).id;
  tracks_[key] = track;
  return track;
}

TrackId TrackTracker::LegacyInternLegacyEnergyPerUidCounterTrack(
    StringId name,
    int32_t consumer_id,
    int32_t uid) {
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendUid(uid);
  dims_builder.AppendDimension(
      context_->storage->InternString("energy_consumer_id"),
      Variadic::Integer(consumer_id));
  Dimensions dims_id = std::move(dims_builder).Build();

  TrackMapKey key;
  key.classification = TrackClassification::kUnknown;
  key.dimensions = dims_id;
  key.name = name;

  auto it = tracks_.Find(key);
  if (it) {
    return *it;
  }

  tables::EnergyPerUidCounterTrackTable::Row row(name);
  row.consumer_id = consumer_id;
  row.uid = uid;
  row.machine_id = context_->machine_id();
  row.dimensions = dims_id.arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());

  TrackId track =
      context_->storage->mutable_energy_per_uid_counter_track_table()
          ->Insert(row)
          .id;
  tracks_[key] = track;
  return track;
}

TrackId TrackTracker::LegacyInternLinuxDeviceTrack(StringId name) {
  Dimensions dims_id = SingleDimension(name_id_, Variadic::String(name));

  if (auto it = tracks_.Find({TrackClassification::kLinuxDevice, dims_id});
      it) {
    return *it;
  }

  tables::LinuxDeviceTrackTable::Row row(name);
  row.dimensions = dims_id.arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kLinuxDevice).c_str());

  TrackId track =
      context_->storage->mutable_linux_device_track_table()->Insert(row).id;
  tracks_[{TrackClassification::kLinuxDevice, dims_id}] = track;
  return track;
}

TrackId TrackTracker::LegacyCreateGpuCounterTrack(StringId name,
                                                  uint32_t gpu_id,
                                                  StringId description,
                                                  StringId unit) {
  tables::GpuCounterTrackTable::Row row(name);
  row.gpu_id = gpu_id;
  row.description = description;
  row.unit = unit;
  row.machine_id = context_->machine_id();
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());
  row.dimensions =
      SingleDimension(gpu_id_, Variadic::Integer(gpu_id)).arg_set_id;

  return context_->storage->mutable_gpu_counter_track_table()->Insert(row).id;
}

TrackId TrackTracker::LegacyCreatePerfCounterTrack(
    StringId name,
    tables::PerfSessionTable::Id perf_session_id,
    uint32_t cpu,
    bool is_timebase) {
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendCpu(cpu);
  dims_builder.AppendDimension(
      context_->storage->InternString("perf_session_id"),
      Variadic::Integer(perf_session_id.value));
  Dimensions dims_id = std::move(dims_builder).Build();

  tables::PerfCounterTrackTable::Row row(name);
  row.perf_session_id = perf_session_id;
  row.cpu = cpu;
  row.is_timebase = is_timebase;
  row.dimensions = dims_id.arg_set_id;
  row.classification = context_->storage->InternString(
      GetClassification(TrackClassification::kUnknown).c_str());
  row.machine_id = context_->machine_id();
  return context_->storage->mutable_perf_counter_track_table()->Insert(row).id;
}

TrackId TrackTracker::InternTrackForGroup(TrackTracker::Group group) {
  uint32_t group_idx = static_cast<uint32_t>(group);
  const std::optional<TrackId>& group_id = group_track_ids_[group_idx];
  if (group_id) {
    return *group_id;
  }

  StringId name_id = context_->storage->InternString(GetNameForGroup(group));
  TrackId track_id =
      InternTrack(TrackClassification::kUnknown, std::nullopt, name_id);
  group_track_ids_[group_idx] = track_id;
  return track_id;
}

}  // namespace trace_processor
}  // namespace perfetto
