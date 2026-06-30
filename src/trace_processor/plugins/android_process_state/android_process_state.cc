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

#include "src/trace_processor/plugins/android_process_state/android_process_state.h"

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
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::android_process_state {
namespace {

// Owns the process-state-change table and the tracker shared by the two hooks
// that fill it: the dump proto importer module and the delta extension parser.
class AndroidProcessState : public Plugin<AndroidProcessState> {
 public:
  ~AndroidProcessState() override;

  void RegisterDataframes(std::vector<PluginDataframe>& out) override {
    EnsureTable();
    out.push_back({&change_table_->dataframe(),
                   tables::AndroidProcessStateChangeTable::Name(),
                   {}});
  }

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    module_context->modules.emplace_back(new AndroidProcessStateModule(
        module_context, EnsureTracker(trace_context)));
  }

  void RegisterTrackEventExtensions(
      TrackEventExtensionParserContext* context,
      TraceProcessorContext* trace_context) override {
    context->parsers.emplace_back(new AndroidProcessStateExtensionParser(
        context, trace_context, EnsureTracker(trace_context)));
  }

  uint64_t GetBoundsMutationCount() override {
    return change_table_ ? change_table_->mutations() : 0;
  }

  std::pair<int64_t, int64_t> GetTimestampBounds() override {
    int64_t start_ns = std::numeric_limits<int64_t>::max();
    int64_t end_ns = 0;
    if (change_table_) {
      for (auto it = change_table_->IterateRows(); it; ++it) {
        if (it.ts()) {  // initial-state rows have no timestamp.
          start_ns = std::min(*it.ts(), start_ns);
          end_ns = std::max(*it.ts(), end_ns);
        }
      }
    }
    return {start_ns, end_ns};
  }

 private:
  void EnsureTable() {
    if (!change_table_) {
      change_table_ = std::make_unique<tables::AndroidProcessStateChangeTable>(
          trace_context_->storage->mutable_string_pool());
    }
  }

  // Uses the parsing context from the registration callback: process_tracker is
  // populated there, not on the plugin's back-pointer context.
  AndroidProcessStateTracker* EnsureTracker(TraceProcessorContext* ctx) {
    EnsureTable();
    if (!tracker_) {
      tracker_ = std::make_unique<AndroidProcessStateTracker>(
          ctx, change_table_.get());
    }
    return tracker_.get();
  }

  std::unique_ptr<tables::AndroidProcessStateChangeTable> change_table_;
  std::unique_ptr<AndroidProcessStateTracker> tracker_;
};

AndroidProcessState::~AndroidProcessState() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<AndroidProcessState>();
      },
      AndroidProcessState::kPluginId, AndroidProcessState::kDepIds.data(),
      AndroidProcessState::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::android_process_state
