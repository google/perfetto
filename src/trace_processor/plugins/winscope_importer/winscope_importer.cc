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

#include "src/trace_processor/plugins/winscope_importer/winscope_importer.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::winscope_importer {

class WinscopeImporter : public Plugin<WinscopeImporter> {
 public:
  ~WinscopeImporter() override;

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    module_context->modules.emplace_back(
        new WinscopeModule(module_context, trace_context));
  }

  // The winscope snapshot tables are the only events in a trace captured with
  // just the winscope data sources, so this plugin contributes their extent to
  // the trace bounds (otherwise such a trace has empty bounds and the UI treats
  // it as empty). GetBoundsMutationCount must enumerate the same tables.
  uint64_t GetBoundsMutationCount() override {
    if (trace_context_ == nullptr) {
      return 0;
    }
    const auto& s = *trace_context_->storage;
    return s.surfaceflinger_layers_snapshot_table().mutations() +
           s.surfaceflinger_transactions_table().mutations() +
           s.windowmanager_table().mutations() +
           s.window_manager_shell_transitions_table().mutations() +
           s.inputmethod_clients_table().mutations() +
           s.inputmethod_manager_service_table().mutations() +
           s.inputmethod_service_table().mutations() +
           s.viewcapture_table().mutations() + s.protolog_table().mutations();
  }

  std::pair<int64_t, int64_t> GetTimestampBounds() override {
    int64_t start_ns = std::numeric_limits<int64_t>::max();
    int64_t end_ns = 0;
    if (trace_context_ == nullptr) {
      return {start_ns, end_ns};
    }
    const auto& s = *trace_context_->storage;
    const auto include = [&](auto it) {
      for (; it; ++it) {
        start_ns = std::min(it.ts(), start_ns);
        end_ns = std::max(it.ts(), end_ns);
      }
    };
    include(s.surfaceflinger_layers_snapshot_table().IterateRows());
    include(s.surfaceflinger_transactions_table().IterateRows());
    include(s.windowmanager_table().IterateRows());
    include(s.window_manager_shell_transitions_table().IterateRows());
    include(s.inputmethod_clients_table().IterateRows());
    include(s.inputmethod_manager_service_table().IterateRows());
    include(s.inputmethod_service_table().IterateRows());
    include(s.viewcapture_table().IterateRows());
    include(s.protolog_table().IterateRows());
    return {start_ns, end_ns};
  }
};

WinscopeImporter::~WinscopeImporter() = default;

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<WinscopeImporter>();
      },
      WinscopeImporter::kPluginId, WinscopeImporter::kDepIds.data(),
      WinscopeImporter::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::winscope_importer
