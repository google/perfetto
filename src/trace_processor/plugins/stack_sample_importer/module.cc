/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/stack_sample_importer/module.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/ext/base/fnv_hash.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/stack_profile_sequence_state.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/stack_sample.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"

namespace perfetto::trace_processor::stack_sample_importer {
namespace {

using perfetto::protos::pbzero::TracePacket;

const char* StringifyStackSampleMode(protos::pbzero::StackSample::Mode mode) {
  using StackSample = protos::pbzero::StackSample;
  switch (mode) {
    case StackSample::Mode::MODE_UNKNOWN:
      return "";
    case StackSample::Mode::MODE_USER:
      return "user";
    case StackSample::Mode::MODE_KERNEL:
      return "kernel";
    case StackSample::Mode::MODE_HYPERVISOR:
      return "hypervisor";
    case StackSample::Mode::MODE_GUEST_USER:
      return "guest_user";
    case StackSample::Mode::MODE_GUEST_KERNEL:
      return "guest_kernel";
  }
  return "";
}

const char* StringifyProfileUnit(protos::pbzero::StackSample::Unit unit) {
  using StackSample = protos::pbzero::StackSample;
  switch (unit) {
    case StackSample::Unit::UNIT_UNSPECIFIED:
      return "";
    case StackSample::Unit::UNIT_NANOSECONDS:
      return "ns";
    case StackSample::Unit::UNIT_CPU_CYCLES:
      return "cycles";
    case StackSample::Unit::UNIT_INSTRUCTIONS:
      return "instructions";
    case StackSample::Unit::UNIT_BYTES:
      return "bytes";
    case StackSample::Unit::UNIT_PAGE_FAULTS:
      return "page-faults";
    case StackSample::Unit::UNIT_CACHE_MISSES:
      return "cache-misses";
    case StackSample::Unit::UNIT_CACHE_REFERENCES:
      return "cache-references";
    case StackSample::Unit::UNIT_BRANCH_MISSES:
      return "branch-misses";
    case StackSample::Unit::UNIT_COUNT:
      return "count";
  }
  return "";
}

uint64_t OptStringKey(std::optional<StringId> id) {
  return id ? (uint64_t{1} << 32) | id->raw_id() : 0;
}

uint64_t OptUintKey(std::optional<uint32_t> v) {
  return v ? (uint64_t{1} << 32) | *v : 0;
}

ResolvedCounterDescriptor ResolveCounterDescriptor(
    TraceProcessorContext* context,
    const protos::pbzero::StackSample::CounterDescriptor::Decoder& desc) {
  using StackSample = protos::pbzero::StackSample;
  ResolvedCounterDescriptor out;
  out.name = context->storage->InternString(desc.name());
  if (desc.has_unit() && desc.unit() != StackSample::Unit::UNIT_UNSPECIFIED) {
    out.unit = context->storage->InternString(
        StringifyProfileUnit(static_cast<StackSample::Unit>(desc.unit())));
  } else if (desc.unit_str().size > 0) {
    out.unit = context->storage->InternString(desc.unit_str());
  }
  if (desc.has_unit_multiplier()) {
    out.unit_multiplier = static_cast<int64_t>(desc.unit_multiplier());
  }
  if (desc.description().size > 0) {
    out.description = context->storage->InternString(desc.description());
  }
  return out;
}

// Resolves an inline callstack (a profile_common Callstack whose frame_ids are
// interned frame iids).
std::optional<CallsiteId> ResolveCallstack(
    PacketSequenceStateGeneration* sequence_state,
    std::optional<UniquePid> upid,
    const protos::pbzero::StackSample::Decoder& sample) {
  if (!sample.has_callstack()) {
    return std::nullopt;
  }
  auto* state = sequence_state->GetCustomState<StackProfileSequenceState>();
  protos::pbzero::Callstack::Decoder callstack(sample.callstack());
  return state->FindOrInsertCallstackFromFrames(sequence_state, upid,
                                                callstack);
}

}  // namespace

StackSampleModule::StackSampleModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context,
    tables::StackSampleTable* table,
    tables::StackSampleTaskContextTable* task_context_table,
    tables::StackSampleExecutionContextTable* exec_context_table,
    tables::StackSampleCounterTable* counter_table,
    tables::StackSampleFollowerTable* follower_table)
    : ProtoImporterModule(module_context),
      context_(context),
      table_(table),
      task_context_table_(task_context_table),
      exec_context_table_(exec_context_table),
      counter_table_(counter_table),
      follower_table_(follower_table) {
  RegisterForField(TracePacket::kStackSampleFieldNumber);
}

void StackSampleModule::ParseField(const ParseFieldArgs& args) {
  if (args.field.id() == TracePacket::kStackSampleFieldNumber) {
    ParseStackSample(args.ts, args.data.sequence_state.get(),
                     args.field.Cast<TracePacket::kStackSample>());
  }
}

tables::StackSampleTaskContextTable::Id StackSampleModule::InternTaskContext(
    std::optional<uint32_t> utid,
    std::optional<uint32_t> upid,
    std::optional<StringId> async_name,
    std::optional<StringId> async_kind) {
  uint64_t key = base::FnvHasher::Combine(OptUintKey(utid), OptUintKey(upid),
                                          OptStringKey(async_name),
                                          OptStringKey(async_kind));
  if (auto* id = task_contexts_.Find(key)) {
    return *id;
  }
  tables::StackSampleTaskContextTable::Row row;
  row.utid = utid;
  row.upid = upid;
  row.async_name = async_name;
  row.async_kind = async_kind;
  auto id = task_context_table_->Insert(row).id;
  task_contexts_.Insert(key, id);
  return id;
}

tables::StackSampleExecutionContextTable::Id
StackSampleModule::InternExecutionContext(std::optional<uint32_t> cpu,
                                          StringId mode) {
  uint64_t key = base::FnvHasher::Combine(OptUintKey(cpu), mode.raw_id());
  if (auto* id = exec_contexts_.Find(key)) {
    return *id;
  }
  tables::StackSampleExecutionContextTable::Row row;
  row.cpu = cpu;
  row.mode = mode;
  auto id = exec_context_table_->Insert(row).id;
  exec_contexts_.Insert(key, id);
  return id;
}

tables::StackSampleCounterTable::Id StackSampleModule::InternCounter(
    StringId source,
    const ResolvedCounterDescriptor& desc) {
  uint64_t key = base::FnvHasher::Combine(
      source.raw_id(), desc.name.raw_id(), OptStringKey(desc.unit),
      desc.unit_multiplier ? static_cast<uint64_t>(*desc.unit_multiplier) : 0,
      OptStringKey(desc.description));
  if (auto* id = counters_.Find(key)) {
    return *id;
  }
  tables::StackSampleCounterTable::Row row;
  row.source = source;
  row.name = desc.name;
  row.unit = desc.unit;
  row.unit_multiplier = desc.unit_multiplier;
  row.description = desc.description;
  auto id = counter_table_->Insert(row).id;
  counters_.Insert(key, id);
  return id;
}

void StackSampleModule::ParseFollowers(
    tables::StackSampleTable::Id sample_id,
    PacketSequenceStateGeneration* sequence_state,
    StringId source,
    const protos::pbzero::StackSample::Decoder& sample,
    const std::optional<protos::pbzero::StackSampleDefaults::Decoder>&
        defaults) {
  using protos::pbzero::InternedData;
  using CounterDescriptor = protos::pbzero::StackSample::CounterDescriptor;

  std::vector<tables::StackSampleCounterTable::Id> counters;
  for (auto it = sample.follower_descriptors(); it; ++it) {
    CounterDescriptor::Decoder desc(*it);
    counters.push_back(
        InternCounter(source, ResolveCounterDescriptor(context_, desc)));
  }
  bool parse_error = false;
  if (counters.empty()) {
    for (auto it = sample.follower_descriptor_iids(&parse_error); it; ++it) {
      auto* desc = sequence_state->LookupInternedMessage<
          InternedData::kStackSampleCounterDescriptorsFieldNumber,
          CounterDescriptor>(*it);
      if (!desc) {
        continue;
      }
      counters.push_back(
          InternCounter(source, ResolveCounterDescriptor(context_, *desc)));
    }
  }
  if (counters.empty() && defaults) {
    for (auto it = defaults->follower_descriptors(); it; ++it) {
      CounterDescriptor::Decoder desc(*it);
      counters.push_back(
          InternCounter(source, ResolveCounterDescriptor(context_, desc)));
    }
  }

  size_t i = 0;
  for (auto it = sample.follower_weights(&parse_error); it; ++it, ++i) {
    if (i >= counters.size()) {
      break;
    }
    tables::StackSampleFollowerTable::Row row;
    row.stack_sample_id = sample_id;
    row.counter_id = counters[i];
    row.weight = static_cast<int64_t>(*it);
    follower_table_->Insert(row);
  }
}

void StackSampleModule::ParseStackSample(
    int64_t ts,
    PacketSequenceStateGeneration* sequence_state,
    protozero::ConstBytes blob) {
  using protos::pbzero::InternedData;
  using protos::pbzero::StackSample;
  using protos::pbzero::StackSampleDefaults;
  using AsyncContextDescriptor = StackSample::AsyncContextDescriptor;
  using CounterDescriptor = StackSample::CounterDescriptor;
  using ExecutionContext = StackSample::ExecutionContext;
  using Mode = StackSample::Mode;
  using TaskContext = StackSample::TaskContext;

  StackSample::Decoder sample(blob.data, blob.size);

  // Defaults carry the source and the fallback primary descriptor.
  std::optional<StackSampleDefaults::Decoder> defaults;
  if (auto* d = sequence_state->GetTracePacketDefaults();
      d && d->has_stack_sample_defaults()) {
    defaults.emplace(d->stack_sample_defaults());
  }
  StringId source_id = kNullStringId;
  if (defaults && defaults->source().size > 0) {
    source_id = context_->storage->InternString(defaults->source());
  }

  // Task context: attributes the sample to a thread / process / async context.
  std::optional<uint32_t> pid;
  std::optional<uint32_t> tid;
  std::optional<uint64_t> async_id;
  bool has_task = false;
  auto extract_task = [&](const TaskContext::Decoder& t) {
    has_task = true;
    if (t.has_pid()) {
      pid = t.pid();
    }
    if (t.has_tid()) {
      tid = t.tid();
    }
    if (t.has_async_id()) {
      async_id = t.async_id();
    }
  };
  if (sample.has_task_context()) {
    TaskContext::Decoder t(sample.task_context());
    extract_task(t);
  } else if (sample.has_task_context_iid()) {
    if (auto* t =
            sequence_state->LookupInternedMessage<
                InternedData::kStackSampleTaskContextsFieldNumber, TaskContext>(
                sample.task_context_iid())) {
      extract_task(*t);
    }
  }

  ProcessTracker* procs = context_->process_tracker.get();
  std::optional<UniquePid> upid;
  std::optional<uint32_t> utid;
  if (pid) {
    upid = procs->GetOrCreateProcess(*pid);
  }
  if (tid) {
    utid =
        pid ? procs->UpdateThread(*tid, *pid) : procs->GetOrCreateThread(*tid);
  }
  // async_id references an interned AsyncContextDescriptor; fold its name and
  // kind onto the task context.
  std::optional<StringId> async_name;
  std::optional<StringId> async_kind;
  if (async_id) {
    if (auto* desc =
            sequence_state->LookupInternedMessage<
                InternedData::kStackSampleAsyncContextDescriptorsFieldNumber,
                AsyncContextDescriptor>(*async_id)) {
      if (desc->name().size > 0) {
        async_name = context_->storage->InternString(desc->name());
      }
      if (desc->kind().size > 0) {
        async_kind = context_->storage->InternString(desc->kind());
      }
    }
  }
  std::optional<tables::StackSampleTaskContextTable::Id> task_context_id;
  if (has_task && (utid || upid || async_name || async_kind)) {
    task_context_id = InternTaskContext(utid, upid, async_name, async_kind);
  }

  // Execution context: cpu + privilege mode at sample time.
  std::optional<uint32_t> cpu;
  Mode mode = StackSample::Mode::MODE_UNKNOWN;
  bool has_exec = false;
  auto extract_exec = [&](const ExecutionContext::Decoder& e) {
    has_exec = true;
    if (e.has_cpu()) {
      cpu = e.cpu();
    }
    if (e.has_mode()) {
      mode = static_cast<Mode>(e.mode());
    }
  };
  if (sample.has_execution_context()) {
    ExecutionContext::Decoder e(sample.execution_context());
    extract_exec(e);
  } else if (sample.has_execution_context_iid()) {
    if (auto* e = sequence_state->LookupInternedMessage<
                  InternedData::kStackSampleExecutionContextsFieldNumber,
                  ExecutionContext>(sample.execution_context_iid())) {
      extract_exec(*e);
    }
  }
  std::optional<tables::StackSampleExecutionContextTable::Id>
      execution_context_id;
  if (has_exec) {
    StringId mode_id =
        context_->storage->InternString(StringifyStackSampleMode(mode));
    execution_context_id = InternExecutionContext(cpu, mode_id);
  }

  // Primary (timebase) descriptor: inline, interned, or from defaults.
  ResolvedCounterDescriptor primary;
  if (sample.has_primary_descriptor()) {
    CounterDescriptor::Decoder d(sample.primary_descriptor());
    primary = ResolveCounterDescriptor(context_, d);
  } else if (sample.has_primary_descriptor_iid()) {
    if (auto* d = sequence_state->LookupInternedMessage<
                  InternedData::kStackSampleCounterDescriptorsFieldNumber,
                  CounterDescriptor>(sample.primary_descriptor_iid())) {
      primary = ResolveCounterDescriptor(context_, *d);
    }
  } else if (defaults && defaults->has_primary_descriptor()) {
    CounterDescriptor::Decoder d(defaults->primary_descriptor());
    primary = ResolveCounterDescriptor(context_, d);
  }
  tables::StackSampleCounterTable::Id timebase_id =
      InternCounter(source_id, primary);

  std::optional<CallsiteId> cs_id =
      ResolveCallstack(sequence_state, upid, sample);

  std::optional<int64_t> weight;
  if (sample.has_primary_weight()) {
    weight = static_cast<int64_t>(sample.primary_weight());
  }

  tables::StackSampleTable::Row row;
  row.ts = ts;
  row.task_context_id = task_context_id;
  row.execution_context_id = execution_context_id;
  row.timebase_id = timebase_id;
  row.callsite_id = cs_id;
  row.weight = weight;
  tables::StackSampleTable::Id sample_id = table_->Insert(row).id;

  ParseFollowers(sample_id, sequence_state, source_id, sample, defaults);
}

}  // namespace perfetto::trace_processor::stack_sample_importer
