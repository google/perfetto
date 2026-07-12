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

#include "src/trace_processor/plugins/stack_sample_importer/plugin.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/fnv_hash.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/stack_profile_sequence_state.h"
#include "src/trace_processor/plugins/stack_sample_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/stack_sample.pbzero.h"
#include "protos/perfetto/trace/profiling/stack_sample_interned_data.pbzero.h"
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

// A resolved StackSample.CounterDescriptor: all strings already interned.
struct ResolvedCounterDescriptor {
  StringId name = kNullStringId;
  std::optional<StringId> unit;
  std::optional<int64_t> unit_multiplier;
  std::optional<StringId> description;
};

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
template <typename Decoder>
std::optional<CallsiteId> ResolveCallstack(
    PacketSequenceStateGeneration* sequence_state,
    std::optional<UniquePid> upid,
    const Decoder& ev) {
  if (!ev.has_callstack()) {
    return std::nullopt;
  }
  auto* state = sequence_state->GetCustomState<StackProfileSequenceState>();
  protos::pbzero::Callstack::Decoder callstack(ev.callstack());
  return state->FindOrInsertCallstackFromFrames(sequence_state, upid,
                                                callstack);
}

// Parses the transport-neutral StackSample packets (see
// profiling/stack_sample.proto) into the plugin-owned tables, passed in by the
// owning plugin. The task, execution and timebase contexts are deduplicated
// into their own tables; the sample rows reference them by id.
class StackSampleModule : public ProtoImporterModule {
 public:
  StackSampleModule(ProtoImporterModuleContext* module_context,
                    TraceProcessorContext* context,
                    tables::StackSampleTable* table,
                    tables::StackSampleTaskContextTable* task_context_table,
                    tables::StackSampleExecutionContextTable* exec_context_table,
                    tables::StackSampleTimebaseTable* timebase_table)
      : ProtoImporterModule(module_context),
        context_(context),
        table_(table),
        task_context_table_(task_context_table),
        exec_context_table_(exec_context_table),
        timebase_table_(timebase_table) {
    RegisterForField(TracePacket::kStackSampleFieldNumber);
  }

  void ParseField(const ParseFieldArgs& args) override {
    if (args.field.id() == TracePacket::kStackSampleFieldNumber) {
      ParseStackSample(args.ts, args.data.sequence_state.get(),
                       args.field.Cast<TracePacket::kStackSample>());
    }
  }

 private:
  tables::StackSampleTaskContextTable::Id InternTaskContext(
      std::optional<uint32_t> utid,
      std::optional<uint32_t> upid) {
    uint64_t key = base::FnvHasher::Combine(OptUintKey(utid), OptUintKey(upid));
    if (auto* id = task_contexts_.Find(key)) {
      return *id;
    }
    tables::StackSampleTaskContextTable::Row row;
    row.utid = utid;
    row.upid = upid;
    auto id = task_context_table_->Insert(row).id;
    task_contexts_.Insert(key, id);
    return id;
  }

  tables::StackSampleExecutionContextTable::Id InternExecutionContext(
      std::optional<uint32_t> cpu,
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

  tables::StackSampleTimebaseTable::Id InternTimebase(
      StringId source,
      const ResolvedCounterDescriptor& desc) {
    uint64_t key = base::FnvHasher::Combine(
        source.raw_id(), desc.name.raw_id(), OptStringKey(desc.unit),
        desc.unit_multiplier ? static_cast<uint64_t>(*desc.unit_multiplier) : 0,
        OptStringKey(desc.description));
    if (auto* id = timebases_.Find(key)) {
      return *id;
    }
    tables::StackSampleTimebaseTable::Row row;
    row.source = source;
    row.name = desc.name;
    row.unit = desc.unit;
    row.unit_multiplier = desc.unit_multiplier;
    row.description = desc.description;
    auto id = timebase_table_->Insert(row).id;
    timebases_.Insert(key, id);
    return id;
  }

  void ParseStackSample(int64_t ts,
                        PacketSequenceStateGeneration* sequence_state,
                        protozero::ConstBytes blob) {
    using protos::pbzero::StackSample;
    using protos::pbzero::StackSampleDefaults;
    using protos::pbzero::StackSampleInternedData;
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

    // Task context: attributes the sample to a thread / process.
    std::optional<uint32_t> pid;
    std::optional<uint32_t> tid;
    bool has_task = false;
    auto extract_task = [&](const TaskContext::Decoder& t) {
      has_task = true;
      if (t.has_pid()) {
        pid = t.pid();
      }
      if (t.has_tid()) {
        tid = t.tid();
      }
    };
    if (sample.has_task_context()) {
      TaskContext::Decoder t(sample.task_context());
      extract_task(t);
    } else if (sample.has_task_context_iid()) {
      if (auto* t = sequence_state->LookupInternedMessage<
              StackSampleInternedData::kTaskContextsFieldNumber, TaskContext>(
              sample.task_context_iid())) {
        extract_task(*t);
      }
    }

    ProcessTracker* procs = context_->process_tracker.get();
    std::optional<UniquePid> upid;
    std::optional<uint32_t> utid;
    if (pid) {
      upid = procs->GetOrCreateProcess(*pid);
      if (tid) {
        utid = procs->UpdateThread(*tid, *pid);
      }
    }
    std::optional<tables::StackSampleTaskContextTable::Id> task_context_id;
    if (has_task && (utid || upid)) {
      task_context_id = InternTaskContext(utid, upid);
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
              StackSampleInternedData::kExecutionContextsFieldNumber,
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
              StackSampleInternedData::kCounterDescriptorsFieldNumber,
              CounterDescriptor>(sample.primary_descriptor_iid())) {
        primary = ResolveCounterDescriptor(context_, *d);
      }
    } else if (defaults && defaults->has_primary_descriptor()) {
      CounterDescriptor::Decoder d(defaults->primary_descriptor());
      primary = ResolveCounterDescriptor(context_, d);
    }
    tables::StackSampleTimebaseTable::Id timebase_id =
        InternTimebase(source_id, primary);

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
    table_->Insert(row);
  }

  TraceProcessorContext* const context_;
  tables::StackSampleTable* const table_;
  tables::StackSampleTaskContextTable* const task_context_table_;
  tables::StackSampleExecutionContextTable* const exec_context_table_;
  tables::StackSampleTimebaseTable* const timebase_table_;

  // Content-dedup maps: fingerprint of the context fields -> interned row id.
  base::FlatHashMap<uint64_t, tables::StackSampleTaskContextTable::Id>
      task_contexts_;
  base::FlatHashMap<uint64_t, tables::StackSampleExecutionContextTable::Id>
      exec_contexts_;
  base::FlatHashMap<uint64_t, tables::StackSampleTimebaseTable::Id> timebases_;
};

// The plugin owns the __intrinsic_stack_sample table and its three context
// tables, populated by StackSampleModule during parsing and living for the
// whole session.
class StackSampleImporter : public Plugin<StackSampleImporter> {
 public:
  ~StackSampleImporter() override;

  void RegisterDataframes(std::vector<PluginDataframe>& out) override {
    EnsureTables();
    out.push_back({&table_->dataframe(), tables::StackSampleTable::Name(), {}});
    out.push_back({&task_context_table_->dataframe(),
                   tables::StackSampleTaskContextTable::Name(), {}});
    out.push_back({&exec_context_table_->dataframe(),
                   tables::StackSampleExecutionContextTable::Name(), {}});
    out.push_back({&timebase_table_->dataframe(),
                   tables::StackSampleTimebaseTable::Name(), {}});
  }

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    EnsureTables();
    module_context->modules.emplace_back(new StackSampleModule(
        module_context, trace_context, table_.get(), task_context_table_.get(),
        exec_context_table_.get(), timebase_table_.get()));
  }

  uint64_t GetBoundsMutationCount() override {
    return table_ ? table_->mutations() : 0;
  }

  std::pair<int64_t, int64_t> GetTimestampBounds() override {
    int64_t start_ns = std::numeric_limits<int64_t>::max();
    int64_t end_ns = 0;
    if (table_) {
      for (auto it = table_->IterateRows(); it; ++it) {
        start_ns = std::min(it.ts(), start_ns);
        end_ns = std::max(it.ts(), end_ns);
      }
    }
    return {start_ns, end_ns};
  }

 private:
  void EnsureTables() {
    if (table_) {
      return;
    }
    auto* pool = trace_context_->storage->mutable_string_pool();
    table_ = std::make_unique<tables::StackSampleTable>(pool);
    task_context_table_ =
        std::make_unique<tables::StackSampleTaskContextTable>(pool);
    exec_context_table_ =
        std::make_unique<tables::StackSampleExecutionContextTable>(pool);
    timebase_table_ =
        std::make_unique<tables::StackSampleTimebaseTable>(pool);
  }

  std::unique_ptr<tables::StackSampleTable> table_;
  std::unique_ptr<tables::StackSampleTaskContextTable> task_context_table_;
  std::unique_ptr<tables::StackSampleExecutionContextTable> exec_context_table_;
  std::unique_ptr<tables::StackSampleTimebaseTable> timebase_table_;
};

StackSampleImporter::~StackSampleImporter() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<StackSampleImporter>();
      },
      StackSampleImporter::kPluginId, StackSampleImporter::kDepIds.data(),
      StackSampleImporter::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::stack_sample_importer
