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

#include "src/trace_processor/plugins/stack_sample_importer/plugin.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/stack_sample_importer/module.h"
#include "src/trace_processor/plugins/stack_sample_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::stack_sample_importer {
namespace {

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
                   tables::StackSampleTaskContextTable::Name(),
                   {}});
    out.push_back({&exec_context_table_->dataframe(),
                   tables::StackSampleExecutionContextTable::Name(),
                   {}});
    out.push_back({&timebase_table_->dataframe(),
                   tables::StackSampleTimebaseTable::Name(),
                   {}});
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
    if (!table_ || table_->row_count() == 0) {
      return {std::numeric_limits<int64_t>::max(), 0};
    }
    // ts is ColumnFlag.SORTED: the first row holds the min, the last the max.
    uint32_t last = table_->row_count() - 1;
    return {(*table_)[0].ts(), (*table_)[last].ts()};
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
    timebase_table_ = std::make_unique<tables::StackSampleTimebaseTable>(pool);
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
