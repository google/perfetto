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

#include "src/trace_processor/plugins/process_state_importer/process_state_importer.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/process_state_importer/process_state_module.h"
#include "src/trace_processor/plugins/process_state_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::process_state_importer {
namespace {

// Owns the six intrinsic tables (snapshot + process / service / provider nodes
// and their binding edges) that ProcessStateModule fills while parsing
// ProcessStateSnapshot packets. The tables live for the whole session.
class ProcessStateImporter : public Plugin<ProcessStateImporter> {
 public:
  ~ProcessStateImporter() override;

  void RegisterDataframes(std::vector<PluginDataframe>& out) override {
    EnsureTables();
    out.push_back({&snapshot_table_->dataframe(),
                   tables::AndroidProcessStateSnapshotTable::Name(),
                   {}});
    out.push_back({&process_table_->dataframe(),
                   tables::AndroidProcessStateProcessTable::Name(),
                   {}});
    out.push_back({&service_table_->dataframe(),
                   tables::AndroidProcessStateServiceTable::Name(),
                   {}});
    out.push_back({&service_binding_table_->dataframe(),
                   tables::AndroidProcessStateServiceBindingTable::Name(),
                   {}});
    out.push_back({&provider_table_->dataframe(),
                   tables::AndroidProcessStateProviderTable::Name(),
                   {}});
    out.push_back({&provider_binding_table_->dataframe(),
                   tables::AndroidProcessStateProviderBindingTable::Name(),
                   {}});
  }

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    EnsureTables();
    module_context->modules.emplace_back(new ProcessStateModule(
        module_context, trace_context, snapshot_table_.get(),
        process_table_.get(), service_table_.get(),
        service_binding_table_.get(), provider_table_.get(),
        provider_binding_table_.get()));
  }

  uint64_t GetBoundsMutationCount() override {
    return snapshot_table_ ? snapshot_table_->mutations() : 0;
  }

  std::pair<int64_t, int64_t> GetTimestampBounds() override {
    int64_t start_ns = std::numeric_limits<int64_t>::max();
    int64_t end_ns = 0;
    if (snapshot_table_) {
      for (auto it = snapshot_table_->IterateRows(); it; ++it) {
        start_ns = std::min(it.ts(), start_ns);
        end_ns = std::max(it.ts(), end_ns);
      }
    }
    return {start_ns, end_ns};
  }

 private:
  void EnsureTables() {
    if (snapshot_table_) {
      return;
    }
    auto* pool = trace_context_->storage->mutable_string_pool();
    snapshot_table_ =
        std::make_unique<tables::AndroidProcessStateSnapshotTable>(pool);
    process_table_ =
        std::make_unique<tables::AndroidProcessStateProcessTable>(pool);
    service_table_ =
        std::make_unique<tables::AndroidProcessStateServiceTable>(pool);
    service_binding_table_ =
        std::make_unique<tables::AndroidProcessStateServiceBindingTable>(pool);
    provider_table_ =
        std::make_unique<tables::AndroidProcessStateProviderTable>(pool);
    provider_binding_table_ =
        std::make_unique<tables::AndroidProcessStateProviderBindingTable>(pool);
  }

  std::unique_ptr<tables::AndroidProcessStateSnapshotTable> snapshot_table_;
  std::unique_ptr<tables::AndroidProcessStateProcessTable> process_table_;
  std::unique_ptr<tables::AndroidProcessStateServiceTable> service_table_;
  std::unique_ptr<tables::AndroidProcessStateServiceBindingTable>
      service_binding_table_;
  std::unique_ptr<tables::AndroidProcessStateProviderTable> provider_table_;
  std::unique_ptr<tables::AndroidProcessStateProviderBindingTable>
      provider_binding_table_;
};

ProcessStateImporter::~ProcessStateImporter() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<ProcessStateImporter>();
      },
      ProcessStateImporter::kPluginId, ProcessStateImporter::kDepIds.data(),
      ProcessStateImporter::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::process_state_importer
