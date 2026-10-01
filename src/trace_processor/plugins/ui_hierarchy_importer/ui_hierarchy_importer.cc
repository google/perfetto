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

#include "src/trace_processor/plugins/ui_hierarchy_importer/ui_hierarchy_importer.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/ui_hierarchy_importer/tables_py.h"
#include "src/trace_processor/plugins/ui_hierarchy_importer/ui_hierarchy_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::ui_hierarchy_importer {
namespace {

// Owns the android.ui.hierarchy tables, populated by UiHierarchyModule.
class UiHierarchyImporter : public Plugin<UiHierarchyImporter> {
 public:
  ~UiHierarchyImporter() override;

  void RegisterDataframes(std::vector<PluginDataframe>& out) override {
    EnsureTables();
    out.push_back({&snapshots_->dataframe(),
                   tables::UiHierarchySnapshotTable::Name(),
                   {}});
    out.push_back(
        {&windows_->dataframe(), tables::UiHierarchyWindowTable::Name(), {}});
    out.push_back(
        {&nodes_->dataframe(), tables::UiHierarchyNodeTable::Name(), {}});
    out.push_back({&window_frames_->dataframe(),
                   tables::UiHierarchyWindowFrameTable::Name(),
                   {}});
    out.push_back(
        {&events_->dataframe(), tables::UiHierarchyEventTable::Name(), {}});
  }

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    EnsureTables();
    module_context->modules.emplace_back(new UiHierarchyModule(
        module_context, trace_context, snapshots_.get(), windows_.get(),
        nodes_.get(), window_frames_.get(), events_.get()));
  }

  uint64_t GetBoundsMutationCount() override {
    return snapshots_ ? snapshots_->mutations() : 0;
  }

  std::pair<int64_t, int64_t> GetTimestampBounds() override {
    int64_t start_ns = std::numeric_limits<int64_t>::max();
    int64_t end_ns = 0;
    if (snapshots_) {
      for (auto it = snapshots_->IterateRows(); it; ++it) {
        start_ns = std::min(it.ts(), start_ns);
        end_ns = std::max(it.ts(), end_ns);
      }
    }
    return {start_ns, end_ns};
  }

 private:
  void EnsureTables() {
    if (snapshots_) {
      return;
    }
    auto* pool = trace_context_->storage->mutable_string_pool();
    snapshots_ = std::make_unique<tables::UiHierarchySnapshotTable>(pool);
    windows_ = std::make_unique<tables::UiHierarchyWindowTable>(pool);
    nodes_ = std::make_unique<tables::UiHierarchyNodeTable>(pool);
    window_frames_ =
        std::make_unique<tables::UiHierarchyWindowFrameTable>(pool);
    events_ = std::make_unique<tables::UiHierarchyEventTable>(pool);
  }

  std::unique_ptr<tables::UiHierarchySnapshotTable> snapshots_;
  std::unique_ptr<tables::UiHierarchyWindowTable> windows_;
  std::unique_ptr<tables::UiHierarchyNodeTable> nodes_;
  std::unique_ptr<tables::UiHierarchyWindowFrameTable> window_frames_;
  std::unique_ptr<tables::UiHierarchyEventTable> events_;
};

UiHierarchyImporter::~UiHierarchyImporter() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<UiHierarchyImporter>();
      },
      UiHierarchyImporter::kPluginId, UiHierarchyImporter::kDepIds.data(),
      UiHierarchyImporter::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::ui_hierarchy_importer
