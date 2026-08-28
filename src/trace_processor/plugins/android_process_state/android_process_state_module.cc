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

#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"

#include <cstdint>

#include "src/trace_processor/plugins/android_process_state/android_process_state_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

namespace perfetto::trace_processor::android_process_state {
namespace {
namespace fb = com::android::internal::pbzero;
}  // namespace

AndroidProcessStateModule::AndroidProcessStateModule(
    ProtoImporterModuleContext* module_context,
    AndroidProcessStateTracker* tracker)
    : ProtoImporterModule(module_context), tracker_(tracker) {
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber);
}

AndroidProcessStateModule::~AndroidProcessStateModule() = default;

void AndroidProcessStateModule::ParseField(const ParseFieldArgs& args) {
  switch (args.field.id()) {
    case fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber:
      tracker_->ParseProcessStateDump(
          args.field
              .Cast<fb::FrameworksBaseTracePacket::kAndroidProcessState>());
      break;
    case fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber:
      tracker_->ParseFreezerDump(
          args.field
              .Cast<fb::FrameworksBaseTracePacket::kAndroidFreezerState>());
      break;
    default:
      break;
  }
}

void AndroidProcessStateModule::OnEventsFullyExtracted() {
  tracker_->Finalize();
}

AndroidProcessStateExtensionParser::AndroidProcessStateExtensionParser(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext* trace_context,
    AndroidProcessStateTracker* tracker)
    : TrackEventExtensionParser(context),
      trace_context_(trace_context),
      tracker_(tracker) {
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber);
}

AndroidProcessStateExtensionParser::~AndroidProcessStateExtensionParser() =
    default;

TrackEventExtensionParser::Result
AndroidProcessStateExtensionParser::OnTrackEventSliceExtension(
    const TrackEventExtensionField& field,
    SliceId id) {
  int64_t ts = trace_context_->storage->slice_table()[id].ts();
  switch (field.id()) {
    case fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber:
      tracker_->ParseProcessStateChange(
          ts,
          field
              .Cast<fb::FrameworksBaseTrackEvent::kProcessStateChangedEvent>());
      break;
    case fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber:
      tracker_->ParseFreezerEvent(
          ts, field.Cast<fb::FrameworksBaseTrackEvent::kFreezerEvent>());
      break;
    default:
      break;
  }
  return Result::kHandled;
}

}  // namespace perfetto::trace_processor::android_process_state
