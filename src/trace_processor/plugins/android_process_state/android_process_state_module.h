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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_MODULE_H_

#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_tracker.h"

namespace perfetto::trace_processor {
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::android_process_state {

// Receives the trace-stop AndroidProcessState and AndroidFreezerState dumps.
class AndroidProcessStateModule : public ProtoImporterModule {
 public:
  AndroidProcessStateModule(ProtoImporterModuleContext* module_context,
                            AndroidProcessStateTracker* tracker);
  ~AndroidProcessStateModule() override;

  void ParseField(const ParseFieldArgs& args) override;
  void OnEventsFullyExtracted() override;

 private:
  AndroidProcessStateTracker* const tracker_;
};

// Receives the per-process AndroidProcessStateChangedEvent and
// AndroidFreezerEvent deltas (TrackEvent extension fields).
class AndroidProcessStateExtensionParser : public TrackEventExtensionParser {
 public:
  AndroidProcessStateExtensionParser(TrackEventExtensionParserContext* context,
                                     TraceProcessorContext* trace_context,
                                     AndroidProcessStateTracker* tracker);
  ~AndroidProcessStateExtensionParser() override;

  Result OnTrackEventSliceExtension(const TrackEventExtensionField& field,
                                    SliceId id) override;

 private:
  TraceProcessorContext* const trace_context_;
  AndroidProcessStateTracker* const tracker_;
};

}  // namespace perfetto::trace_processor::android_process_state

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_MODULE_H_
