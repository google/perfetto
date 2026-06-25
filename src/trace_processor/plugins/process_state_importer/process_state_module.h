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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_PROCESS_STATE_IMPORTER_PROCESS_STATE_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_PROCESS_STATE_IMPORTER_PROCESS_STATE_MODULE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/process_state_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Populates the plugin-owned intrinsic tables (snapshot + process / service /
// provider nodes and their binding edges) two ways:
//   1. From a one-shot ProcessStateSnapshot packet (dumpsys activity): one
//      snapshot row plus all its processes / services / providers / bindings.
//   2. By reconstructing the graph from the Android{Process,Service,Provider}-
//      StateChangedEvent track-event stream: keeps a running model and emits a
//      new snapshot whenever the oom-adj sequence id advances (and a final one
//      at end of trace). Service and provider bindings are tracked by their
//      bind/unbind action + bind_id.
// Enum-valued fields are resolved to their names here (via the generated
// <Enum>_Name() helpers) so the UI keeps no enum tables of its own.
class ProcessStateModule : public ProtoImporterModule {
 public:
  ProcessStateModule(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* context,
      tables::AndroidProcessStateSnapshotTable* snapshot_table,
      tables::AndroidProcessStateProcessTable* process_table,
      tables::AndroidProcessStateServiceTable* service_table,
      tables::AndroidProcessStateServiceBindingTable* service_binding_table,
      tables::AndroidProcessStateProviderTable* provider_table,
      tables::AndroidProcessStateProviderBindingTable* provider_binding_table);
  ~ProcessStateModule() override;

  void ParseField(const ParseFieldArgs& args) override;

  // Flushes the last reconstructed graph at end of trace.
  void OnEventsFullyExtracted() override;

 private:
  // Writes the current running model as a new snapshot at current_ts_.
  void EmitSnapshot();

  // Resolve enum values to interned display names via the generated
  // <Enum>_Name() helpers (the prefix is stripped for brevity).
  StringId ProcStateName(int32_t value);
  StringId ReasonName(int32_t value);
  // A " | "-joined list of the granted ProcessCapabilityEnum names ("none" if
  // the bitmask is zero).
  StringId CapabilityNames(int32_t flags);

  TraceProcessorContext* const context_;
  tables::AndroidProcessStateSnapshotTable* const snapshot_table_;
  tables::AndroidProcessStateProcessTable* const process_table_;
  tables::AndroidProcessStateServiceTable* const service_table_;
  tables::AndroidProcessStateServiceBindingTable* const service_binding_table_;
  tables::AndroidProcessStateProviderTable* const provider_table_;
  tables::AndroidProcessStateProviderBindingTable* const
      provider_binding_table_;

  // Running model of the graph, rebuilt from the track-event stream (and seeded
  // wholesale by a one-shot snapshot). Keyed so repeated events update in
  // place.
  std::map<int32_t, tables::AndroidProcessStateProcessTable::Row> processes_;
  std::map<int32_t, tables::AndroidProcessStateServiceTable::Row> services_;
  std::map<std::pair<int32_t, StringId>, int32_t> service_to_id_;
  std::map<int32_t, tables::AndroidProcessStateServiceBindingTable::Row>
      service_bindings_;
  std::map<int32_t, tables::AndroidProcessStateProviderTable::Row> providers_;
  std::map<std::pair<int32_t, StringId>, int32_t> provider_to_id_;
  std::map<int32_t, tables::AndroidProcessStateProviderBindingTable::Row>
      provider_bindings_;

  int32_t next_svc_id_ = 10000;
  int32_t next_provider_id_ = 20000;
  int64_t current_seq_id_ = -1;
  // A snapshot is stamped at the START of its oom-adj pass (the first event
  // carrying this seq_id); seq_start_ts_ tracks that. current_ts_ is the
  // running last-seen event ts.
  int64_t seq_start_ts_ = -1;
  int64_t current_ts_ = -1;
  // OomChangeReasonEnum of the oom-adj pass currently being accumulated;
  // written onto the snapshot when emitted. Unset for a one-shot dumpsys
  // snapshot.
  std::optional<int32_t> current_reason_;
  bool graph_changed_ = false;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_PROCESS_STATE_IMPORTER_PROCESS_STATE_MODULE_H_
