/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/types/per_trace_context.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

PerTraceContext::PerTraceContext() = default;
PerTraceContext::~PerTraceContext() = default;

void PerTraceContext::Init(TraceProcessorContext* context) {
  global_args_tracker = std::make_shared<GlobalArgsTracker>(
      context->global_context->storage.get());
  args_tracker = std::make_unique<ArgsTracker>(context);
  args_translation_table = std::make_unique<ArgsTranslationTable>(
      context->global_context->storage.get());
  flow_tracker = std::make_unique<FlowTracker>(context);
  event_tracker = std::make_unique<EventTracker>(context);
  trace_file_tracker = std::make_unique<TraceFileTracker>(context);
  stack_profile_tracker = std::make_unique<StackProfileTracker>(context);
  process_track_translation_table =
      std::make_unique<ProcessTrackTranslationTable>(
          context->global_context->storage.get());
  slice_tracker = std::make_unique<SliceTracker>(context);
  slice_translation_table = std::make_unique<SliceTranslationTable>(
      context->global_context->storage.get());
}

PerTraceContext::PerTraceContext(PerTraceContext&&) = default;
PerTraceContext& PerTraceContext::operator=(PerTraceContext&&) = default;

}  // namespace perfetto::trace_processor
