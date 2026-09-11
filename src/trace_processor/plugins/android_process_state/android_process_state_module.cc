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

#include "protos/perfetto/config/android/android_process_state_config.pbzero.h"
#include "protos/perfetto/config/data_source_config.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
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
          args.ts,
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

void AndroidProcessStateModule::TokenizeTraceConfig(
    const protos::pbzero::TraceConfig_Decoder& trace_config) {
  bool ftrace_configured = false;
  std::optional<bool> dump_process_metadata;

  for (auto it = trace_config.data_sources(); it; ++it) {
    protos::pbzero::TraceConfig::DataSource::Decoder ds(*it);
    if (!ds.has_config()) {
      continue;
    }
    protos::pbzero::DataSourceConfig::Decoder cfg(ds.config());
    if (cfg.name().ToStdStringView() == "linux.ftrace") {
      ftrace_configured = true;
    }
    if (cfg.name().ToStdStringView() == "android.process_state") {
      bool dump_meta = false;
      if (cfg.has_android_process_state_config()) {
        protos::pbzero::AndroidProcessStateConfig::Decoder aps_cfg(
            cfg.android_process_state_config());
        dump_meta = aps_cfg.has_dump_process_metadata() &&
                    aps_cfg.dump_process_metadata();
      }
      dump_process_metadata = dump_meta;
    }
  }

  tracker_->OnConfigDetected(ftrace_configured, dump_process_metadata);
}

void AndroidProcessStateModule::OnEventsFullyExtracted() {
  tracker_->Finalize();
}

AndroidProcessStateExtensionParser::AndroidProcessStateExtensionParser(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext*,
    AndroidProcessStateTracker* tracker)
    : TrackEventExtensionParser(context), tracker_(tracker) {
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber);
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
