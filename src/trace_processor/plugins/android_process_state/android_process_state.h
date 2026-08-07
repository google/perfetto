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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_H_

#include <memory>
#include <vector>

#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"

namespace perfetto::trace_processor::android_process_state {

class AndroidProcessState : public Plugin<AndroidProcessState> {
 public:
  ~AndroidProcessState() override;

  void RegisterDataframes(std::vector<PluginDataframe>& out) override;
  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override;
  void RegisterTrackEventExtensions(
      TrackEventExtensionParserContext* context,
      TraceProcessorContext* trace_context) override;

  tables::AndroidProcessStateTable* process_state_table() {
    EnsureTables();
    return process_state_table_.get();
  }

  tables::AndroidFreezerStateTable* freezer_state_table() {
    EnsureTables();
    return freezer_state_table_.get();
  }

 private:
  void EnsureTables();
  AndroidProcessStateTracker* EnsureTracker(TraceProcessorContext* ctx);

  std::unique_ptr<tables::AndroidProcessStateTable> process_state_table_;
  std::unique_ptr<tables::AndroidFreezerStateTable> freezer_state_table_;
  std::unique_ptr<AndroidProcessStateTracker> tracker_;
};

void RegisterPlugin();

}  // namespace perfetto::trace_processor::android_process_state

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_H_
