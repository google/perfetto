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

#include <memory>
#include <vector>

#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::android_process_state {

using tables::AndroidFreezerStateTable;
using tables::AndroidProcessStateTable;

AndroidProcessState::~AndroidProcessState() = default;

void AndroidProcessState::RegisterDataframes(
    std::vector<PluginDataframe>& out) {
  EnsureTables();
  out.push_back({&process_state_table_->dataframe(),
                 AndroidProcessStateTable::Name(),
                 {}});
  out.push_back({&freezer_state_table_->dataframe(),
                 AndroidFreezerStateTable::Name(),
                 {}});
}

void AndroidProcessState::RegisterProtoImporterModules(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* trace_context) {
  EnsureTables();
  module_context->modules.emplace_back(new AndroidProcessStateModule(
      module_context, trace_context, EnsureTracker(trace_context),
      freezer_state_table_.get()));
}

void AndroidProcessState::RegisterTrackEventExtensions(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext* trace_context) {
  EnsureTables();
  context->parsers.emplace_back(new AndroidProcessStateExtensionParser(
      context, trace_context, EnsureTracker(trace_context)));
}

void AndroidProcessState::EnsureTables() {
  if (!process_state_table_) {
    process_state_table_ = std::make_unique<AndroidProcessStateTable>(
        trace_context_->storage->mutable_string_pool());
  }
  if (!freezer_state_table_) {
    freezer_state_table_ = std::make_unique<AndroidFreezerStateTable>(
        trace_context_->storage->mutable_string_pool());
  }
}

AndroidProcessStateTracker* AndroidProcessState::EnsureTracker(
    TraceProcessorContext* ctx) {
  EnsureTables();
  if (!tracker_) {
    tracker_ = std::make_unique<AndroidProcessStateTracker>(
        ctx, process_state_table_.get());
  }
  return tracker_.get();
}

void RegisterPlugin() {
  PERFETTO_TP_REGISTER_PLUGIN(AndroidProcessState);
}

}  // namespace perfetto::trace_processor::android_process_state
