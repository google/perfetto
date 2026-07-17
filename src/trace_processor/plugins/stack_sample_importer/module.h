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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_STACK_SAMPLE_IMPORTER_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_STACK_SAMPLE_IMPORTER_MODULE_H_

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/profiling/stack_sample.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/stack_sample_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {
class PacketSequenceStateGeneration;
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::stack_sample_importer {

// A resolved StackSample.CounterDescriptor: all strings already interned.
struct ResolvedCounterDescriptor {
  StringId name = kNullStringId;
  std::optional<StringId> unit;
  std::optional<int64_t> unit_multiplier;
  std::optional<StringId> description;
};

// Parses the transport-neutral StackSample packets (see
// profiling/stack_sample.proto) into the plugin-owned tables, passed in by the
// owning plugin. The task, execution and timebase contexts are deduplicated
// into their own tables; the sample rows reference them by id.
class StackSampleModule : public ProtoImporterModule {
 public:
  StackSampleModule(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* context,
      tables::StackSampleTable* table,
      tables::StackSampleTaskContextTable* task_context_table,
      tables::StackSampleExecutionContextTable* exec_context_table,
      tables::StackSampleCounterTable* counter_table,
      tables::StackSampleFollowerTable* follower_table);

  void ParseField(const ParseFieldArgs& args) override;

 private:
  tables::StackSampleTaskContextTable::Id InternTaskContext(
      std::optional<uint32_t> utid,
      std::optional<uint32_t> upid);
  tables::StackSampleExecutionContextTable::Id InternExecutionContext(
      std::optional<uint32_t> cpu,
      StringId mode);
  tables::StackSampleCounterTable::Id InternCounter(
      StringId source,
      const ResolvedCounterDescriptor& desc);
  void ParseFollowers(
      tables::StackSampleTable::Id sample_id,
      PacketSequenceStateGeneration* sequence_state,
      StringId source,
      const protos::pbzero::StackSample::Decoder& sample,
      const std::optional<protos::pbzero::StackSampleDefaults::Decoder>&
          defaults);

  void ParseStackSample(int64_t ts,
                        PacketSequenceStateGeneration* sequence_state,
                        protozero::ConstBytes blob);

  TraceProcessorContext* const context_;
  tables::StackSampleTable* const table_;
  tables::StackSampleTaskContextTable* const task_context_table_;
  tables::StackSampleExecutionContextTable* const exec_context_table_;
  tables::StackSampleCounterTable* const counter_table_;
  tables::StackSampleFollowerTable* const follower_table_;

  // Content-dedup maps: fingerprint of the context fields -> interned row id.
  base::FlatHashMap<uint64_t, tables::StackSampleTaskContextTable::Id>
      task_contexts_;
  base::FlatHashMap<uint64_t, tables::StackSampleExecutionContextTable::Id>
      exec_contexts_;
  base::FlatHashMap<uint64_t, tables::StackSampleCounterTable::Id> counters_;
};

}  // namespace perfetto::trace_processor::stack_sample_importer

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_STACK_SAMPLE_IMPORTER_MODULE_H_
