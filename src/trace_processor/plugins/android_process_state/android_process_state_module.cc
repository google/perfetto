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

#include "perfetto/base/compiler.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor::android_process_state {
namespace {
namespace fb = com::android::internal::pbzero;
}  // namespace

AndroidProcessStateModule::AndroidProcessStateModule(
    ProtoImporterModuleContext* module_context,
    AndroidProcessStateTracker* tracker,
    tables::AndroidFreezerStateTable* freezer_state_table)
    : ProtoImporterModule(module_context),
      context_(tracker->context()),
      tracker_(tracker),
      freezer_state_table_(freezer_state_table) {
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber);
}

AndroidProcessStateModule::~AndroidProcessStateModule() = default;

void AndroidProcessStateModule::ParseField(const ParseFieldArgs& args) {
  switch (args.field.id()) {
    case fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber:
      tracker_->ParseDump(
          args.field
              .Cast<fb::FrameworksBaseTracePacket::kAndroidProcessState>());
      break;
    case fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber:
      ParseAndroidFreezerState(
          args.ts,
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

void AndroidProcessStateModule::ParseAndroidFreezerState(
    int64_t ts,
    protozero::ConstBytes blob) {
  base::ignore_result(ts);
  fb::AndroidFreezerStateSnapshot::Decoder evt(blob);
  for (auto it = evt.record(); it; ++it) {
    fb::AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    UniquePid upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(rec.pid()));

    tables::AndroidFreezerStateTable::Row row;
    row.ts = std::nullopt;
    row.upid = upid;
    row.unfrozen_dur_ms = std::nullopt;
    row.frozen_dur_ms = std::nullopt;
    row.unfreeze_reason = std::nullopt;
    row.is_initial = 1;
    freezer_state_table_->Insert(row);
  }
}

AndroidProcessStateExtensionParser::AndroidProcessStateExtensionParser(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext* trace_context,
    AndroidProcessStateTracker* tracker,
    tables::AndroidFreezerStateTable* freezer_state_table)
    : TrackEventExtensionParser(context),
      trace_context_(trace_context),
      tracker_(tracker),
      freezer_state_table_(freezer_state_table) {
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
      tracker_->ParseChange(
          ts,
          field
              .Cast<fb::FrameworksBaseTrackEvent::kProcessStateChangedEvent>());
      break;
    case fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber:
      ParseFreezerEvent(
          ts, field.Cast<fb::FrameworksBaseTrackEvent::kFreezerEvent>());
      break;
    default:
      break;
  }
  return Result::kHandled;
}

void AndroidProcessStateExtensionParser::ParseFreezerEvent(
    int64_t ts,
    protozero::ConstBytes data) {
  fb::AndroidFreezerEvent::Decoder evt(data);
  if (!evt.has_pid()) {
    return;
  }
  tables::AndroidFreezerStateTable::Row row;
  row.ts = ts;
  row.upid = trace_context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(evt.pid()));
  if (evt.has_unfrozen_dur_ms()) {
    row.unfrozen_dur_ms = evt.unfrozen_dur_ms();
  }
  if (evt.has_frozen_dur_ms()) {
    row.frozen_dur_ms = evt.frozen_dur_ms();
  }
  if (evt.has_unfreeze_reason()) {
    row.unfreeze_reason =
        InternEnum(trace_context_, unfreeze_reason_cache_,
                   ".com.android.internal.UnfreezeReason",
                   static_cast<int32_t>(evt.unfreeze_reason()));
  }
  row.is_initial = 0;
  freezer_state_table_->Insert(row);
}

}  // namespace perfetto::trace_processor::android_process_state
