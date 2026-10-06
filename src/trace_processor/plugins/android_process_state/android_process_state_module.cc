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
      fb::FrameworksBaseTrackEvent::kProcessStateDiedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kOomAdjusterPassEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kServiceStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kFgServiceStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProviderStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kBroadcastEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kSelfBroadcastEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kActivityStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProcessStateTriggerEventFieldNumber);
  // AndroidProcessDiedEvent, AndroidBinderDiedEvent and
  // AndroidProcessStartEvent are owned by the android_framework_track_event
  // plugin (a field can only have one parser).
}

AndroidProcessStateExtensionParser::~AndroidProcessStateExtensionParser() =
    default;

TrackEventExtensionParser::Result
AndroidProcessStateExtensionParser::OnTrackEventField(
    const TrackEventExtensionField& field,
    const TrackEventFieldContext& event) {
  if (event.row_kind != TrackEventFieldContext::RowKind::kSlice) {
    return Result::kIgnored;
  }
  int64_t ts = event.ts;
  base::StringView slice_name;
  std::optional<StringId> opt_name =
      trace_context_->storage->slice_table()[event.slice_id()].name();
  if (opt_name) {
    slice_name = trace_context_->storage->GetString(*opt_name);
  }
  switch (field.id()) {
    case fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber:
      tracker_->ParseProcessStateChange(
          ts, event.has_utid() ? std::make_optional(event.utid) : std::nullopt,
          field
              .Cast<fb::FrameworksBaseTrackEvent::kProcessStateChangedEvent>());
      return Result::kHandled;
    case fb::FrameworksBaseTrackEvent::kProcessStateDiedEventFieldNumber:
      tracker_->ParseProcessStateDied(
          ts,
          field.Cast<fb::FrameworksBaseTrackEvent::kProcessStateDiedEvent>());
      return Result::kHandled;
    case fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber:
      tracker_->ParseFreezerEvent(
          ts, field.Cast<fb::FrameworksBaseTrackEvent::kFreezerEvent>());
      return Result::kHandled;
    case fb::FrameworksBaseTrackEvent::kOomAdjusterPassEventFieldNumber:
      tracker_->ParseOomAdjusterPass(
          ts,
          field.Cast<fb::FrameworksBaseTrackEvent::kOomAdjusterPassEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kServiceStateChangedEventFieldNumber:
      tracker_->ParseServiceStateChange(
          ts, slice_name,
          field
              .Cast<fb::FrameworksBaseTrackEvent::kServiceStateChangedEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kFgServiceStateChangedEventFieldNumber:
      tracker_->ParseFgServiceStateChange(
          ts, slice_name,
          field.Cast<
              fb::FrameworksBaseTrackEvent::kFgServiceStateChangedEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kProviderStateChangedEventFieldNumber:
      tracker_->ParseProviderStateChange(
          ts, slice_name,
          field.Cast<
              fb::FrameworksBaseTrackEvent::kProviderStateChangedEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kBroadcastEventFieldNumber:
      tracker_->ParseBroadcastEvent(
          ts, slice_name,
          field.Cast<fb::FrameworksBaseTrackEvent::kBroadcastEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kSelfBroadcastEventFieldNumber:
      tracker_->ParseSelfBroadcastEvent(
          ts, slice_name,
          field.Cast<fb::FrameworksBaseTrackEvent::kSelfBroadcastEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kActivityStateChangedEventFieldNumber:
      tracker_->ParseActivityStateChange(
          ts, field.Cast<
                  fb::FrameworksBaseTrackEvent::kActivityStateChangedEvent>());
      return Result::kIgnored;
    case fb::FrameworksBaseTrackEvent::kProcessStateTriggerEventFieldNumber:
      tracker_->ParseProcessStateTrigger(
          ts,
          field
              .Cast<fb::FrameworksBaseTrackEvent::kProcessStateTriggerEvent>());
      return Result::kIgnored;
    default:
      break;
  }
  return Result::kIgnored;
}

}  // namespace perfetto::trace_processor::android_process_state
