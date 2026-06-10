/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/plugins/winscope_importer/winscope_module.h"

#include <cstdint>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/winscope/frameworks_base_winscope.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/winscope/frameworks_native_winscope.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/args_parser.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/winscope_importer/shell_transitions_tracker.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_proto_mapping.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/tables/winscope_tables_py.h"

namespace perfetto::trace_processor {

using com::android::internal::pbzero::FrameworksBaseWinscopeExtensions;
using com::android::internal::pbzero::FrameworksBaseWinscopeTracePacket;
using com::android::internal::pbzero::FrameworksNativeWinscopeExtensions;
using com::android::internal::pbzero::FrameworksNativeWinscopeTracePacket;
using com::android::internal::pbzero::WinscopeExtensions;
using perfetto::protos::pbzero::TracePacket;

WinscopeModule::WinscopeModule(ProtoImporterModuleContext* module_context,
                               TraceProcessorContext* context)
    : ProtoImporterModule(module_context),
      context_{context},
      args_parser_{*context->descriptor_pool_},
      surfaceflinger_layers_parser_(&context_),
      surfaceflinger_transactions_parser_(context),
      shell_transitions_parser_(&context_),
      protolog_parser_(&context_),
      android_input_event_parser_(context),
      viewcapture_parser_(&context_),
      windowmanager_parser_(&context_) {
  RegisterForField(FrameworksNativeWinscopeTracePacket::
                       kSurfaceflingerLayersSnapshotFieldNumber);
  RegisterForField(FrameworksNativeWinscopeTracePacket::
                       kSurfaceflingerTransactionsFieldNumber);
  RegisterForField(
      FrameworksBaseWinscopeTracePacket::kShellTransitionFieldNumber);
  RegisterForField(
      FrameworksBaseWinscopeTracePacket::kShellHandlerMappingsFieldNumber);
  RegisterForField(
      FrameworksNativeWinscopeTracePacket::kProtologMessageFieldNumber);
  RegisterForField(
      FrameworksNativeWinscopeTracePacket::kProtologViewerConfigFieldNumber);
  RegisterForField(
      FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber);
}

ModuleResult WinscopeModule::TokenizePacket(
    const perfetto::protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView* /*packet*/,
    int64_t /*packet_timestamp*/,
    RefPtr<PacketSequenceStateGeneration> /*state*/,
    uint32_t field_id) {
  switch (field_id) {
    case FrameworksNativeWinscopeTracePacket::kProtologViewerConfigFieldNumber:
      protolog_parser_.ParseAndAddViewerConfigToMessageDecoder(
          decoder
              .GetExtensionSlowly<FrameworksNativeWinscopeTracePacket::
                                      kProtologViewerConfigFieldNumber>()
              .as_bytes());
      return ModuleResult::Handled();
  }

  return ModuleResult::Ignored();
}

void WinscopeModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                          int64_t timestamp,
                                          const TracePacketData& data,
                                          uint32_t field_id) {
  std::optional<uint32_t> sequence_id;
  if (decoder.has_trusted_packet_sequence_id()) {
    sequence_id = decoder.trusted_packet_sequence_id();
  }
  switch (field_id) {
    case FrameworksNativeWinscopeTracePacket::
        kSurfaceflingerLayersSnapshotFieldNumber:
      surfaceflinger_layers_parser_.Parse(
          timestamp,
          decoder
              .GetExtensionSlowly<
                  FrameworksNativeWinscopeTracePacket::
                      kSurfaceflingerLayersSnapshotFieldNumber>()
              .as_bytes(),
          sequence_id);
      return;
    case FrameworksNativeWinscopeTracePacket::
        kSurfaceflingerTransactionsFieldNumber:
      surfaceflinger_transactions_parser_.Parse(
          timestamp,
          decoder
              .GetExtensionSlowly<FrameworksNativeWinscopeTracePacket::
                                      kSurfaceflingerTransactionsFieldNumber>()
              .as_bytes());
      return;
    case FrameworksBaseWinscopeTracePacket::kShellTransitionFieldNumber:
      shell_transitions_parser_.ParseTransition(
          decoder
              .GetExtensionSlowly<FrameworksBaseWinscopeTracePacket::
                                      kShellTransitionFieldNumber>()
              .as_bytes());
      return;
    case FrameworksBaseWinscopeTracePacket::kShellHandlerMappingsFieldNumber:
      shell_transitions_parser_.ParseHandlerMappings(
          decoder
              .GetExtensionSlowly<FrameworksBaseWinscopeTracePacket::
                                      kShellHandlerMappingsFieldNumber>()
              .as_bytes());
      return;
    case FrameworksNativeWinscopeTracePacket::kProtologMessageFieldNumber:
      protolog_parser_.ParseProtoLogMessage(
          data.sequence_state.get(),
          decoder
              .GetExtensionSlowly<FrameworksNativeWinscopeTracePacket::
                                      kProtologMessageFieldNumber>()
              .as_bytes(),
          timestamp);
      return;
    case FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber:
      ParseWinscopeExtensionsData(
          decoder
              .GetExtensionSlowly<FrameworksNativeWinscopeTracePacket::
                                      kWinscopeExtensionsFieldNumber>()
              .as_bytes(),
          timestamp, data);
      return;
  }
}

void WinscopeModule::ParseWinscopeExtensionsData(protozero::ConstBytes blob,
                                                 int64_t timestamp,
                                                 const TracePacketData& data) {
  WinscopeExtensions::Decoder decoder(blob.data, blob.size);
  if (auto field =
          decoder.GetExtensionSlowly<FrameworksBaseWinscopeExtensions::
                                         kInputmethodClientsFieldNumber>();
      field.valid()) {
    ParseInputMethodClientsData(timestamp, field.as_bytes());
  } else if (field = decoder.GetExtensionSlowly<
                     FrameworksBaseWinscopeExtensions::
                         kInputmethodManagerServiceFieldNumber>();
             field.valid()) {
    ParseInputMethodManagerServiceData(timestamp, field.as_bytes());
  } else if (field =
                 decoder
                     .GetExtensionSlowly<FrameworksBaseWinscopeExtensions::
                                             kInputmethodServiceFieldNumber>();
             field.valid()) {
    ParseInputMethodServiceData(timestamp, field.as_bytes());
  } else if (field =
                 decoder.GetExtensionSlowly<FrameworksBaseWinscopeExtensions::
                                                kViewcaptureFieldNumber>();
             field.valid()) {
    viewcapture_parser_.Parse(timestamp, field.as_bytes(),
                              data.sequence_state.get());
  } else if (field =
                 decoder
                     .GetExtensionSlowly<FrameworksNativeWinscopeExtensions::
                                             kAndroidInputEventFieldNumber>();
             field.valid()) {
    android_input_event_parser_.ParseAndroidInputEvent(timestamp,
                                                       field.as_bytes());
  } else if (field =
                 decoder.GetExtensionSlowly<FrameworksBaseWinscopeExtensions::
                                                kWindowmanagerFieldNumber>();
             field.valid()) {
    windowmanager_parser_.Parse(timestamp, field.as_bytes());
  }
}

void WinscopeModule::ParseInputMethodClientsData(int64_t timestamp,
                                                 protozero::ConstBytes blob) {
  auto* trace_processor_context = context_.trace_processor_context_;
  tables::InputMethodClientsTable::Row row;
  row.ts = timestamp;
  row.base64_proto_id = trace_processor_context->storage->mutable_string_pool()
                            ->InternString(base::StringView(
                                base::Base64Encode(blob.data, blob.size)))
                            .raw_id();
  auto rowId =
      trace_processor_context->storage->mutable_inputmethod_clients_table()
          ->Insert(row)
          .id;

  ArgsTracker tracker(trace_processor_context);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *trace_processor_context->storage);
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::InputMethodClientsTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    trace_processor_context->stats_tracker->IncrementStats(
        stats::winscope_inputmethod_clients_parse_errors);
  }
}

void WinscopeModule::ParseInputMethodManagerServiceData(
    int64_t timestamp,
    protozero::ConstBytes blob) {
  auto* trace_processor_context = context_.trace_processor_context_;
  tables::InputMethodManagerServiceTable::Row row;
  row.ts = timestamp;
  row.base64_proto_id = trace_processor_context->storage->mutable_string_pool()
                            ->InternString(base::StringView(
                                base::Base64Encode(blob.data, blob.size)))
                            .raw_id();
  auto rowId = trace_processor_context->storage
                   ->mutable_inputmethod_manager_service_table()
                   ->Insert(row)
                   .id;

  ArgsTracker tracker(trace_processor_context);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *trace_processor_context->storage);
  base::Status status = args_parser_.ParseMessage(
      blob,
      *util::winscope_proto_mapping::GetProtoName(
          tables::InputMethodManagerServiceTable::Name()),
      nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    trace_processor_context->stats_tracker->IncrementStats(
        stats::winscope_inputmethod_manager_service_parse_errors);
  }
}

void WinscopeModule::ParseInputMethodServiceData(int64_t timestamp,
                                                 protozero::ConstBytes blob) {
  auto* trace_processor_context = context_.trace_processor_context_;
  tables::InputMethodServiceTable::Row row;
  row.ts = timestamp;
  row.base64_proto_id = trace_processor_context->storage->mutable_string_pool()
                            ->InternString(base::StringView(
                                base::Base64Encode(blob.data, blob.size)))
                            .raw_id();
  auto rowId =
      trace_processor_context->storage->mutable_inputmethod_service_table()
          ->Insert(row)
          .id;

  ArgsTracker tracker(trace_processor_context);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *trace_processor_context->storage);
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::InputMethodServiceTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    trace_processor_context->stats_tracker->IncrementStats(
        stats::winscope_inputmethod_service_parse_errors);
  }
}

void WinscopeModule::OnEventsFullyExtracted() {
  context_.shell_transitions_tracker_.Flush();
}

}  // namespace perfetto::trace_processor
