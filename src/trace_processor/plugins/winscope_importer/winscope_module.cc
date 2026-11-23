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
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_interned_data.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/winscope/frameworks_base_winscope.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/frameworks_native_interned_data.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/winscope/android_input_event.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/winscope/frameworks_native_winscope.pbzero.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/winscope_importer/shell_transitions_tracker.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_proto_mapping.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/tables/winscope_tables_py.h"

namespace perfetto::trace_processor {

using com::android::internal::pbzero::AndroidInputEvent;
using com::android::internal::pbzero::FrameworksBaseInternedData;
using com::android::internal::pbzero::FrameworksBaseWinscopeExtensions;
using com::android::internal::pbzero::FrameworksBaseWinscopeTracePacket;
using com::android::internal::pbzero::FrameworksNativeInternedData;
using com::android::internal::pbzero::FrameworksNativeWinscopeExtensions;
using com::android::internal::pbzero::FrameworksNativeWinscopeTracePacket;
using com::android::internal::pbzero::WinscopeExtensions;
using perfetto::protos::pbzero::TracePacket;

namespace {

bool IsLengthDelimited(const protozero::Field& field) {
  return field.type() ==
         protozero::proto_utils::ProtoWireType::kLengthDelimited;
}

void RecordProtoVmStateBytes(TraceProcessorContext* context,
                             protozero::ConstBytes state) {
  if (state.size == 0) {
    return;
  }
  const int64_t state_bytes = static_cast<int64_t>(state.size);
  protozero::ProtoDecoder state_decoder(state);
  for (protozero::Field f = state_decoder.ReadField(); f.valid();
       f = state_decoder.ReadField()) {
    if (!IsLengthDelimited(f)) {
      continue;
    }
    if (f.id() == FrameworksNativeWinscopeTracePacket::
                      kSurfaceflingerLayersSnapshotFieldNumber) {
      context->stats_tracker->IncrementStats(
          stats::winscope_sf_layers_protovm_state_bytes, state_bytes);
      return;
    }
    if (f.id() ==
        FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber) {
      protozero::ProtoDecoder ext_decoder(f.as_bytes());
      for (protozero::Field ext = ext_decoder.ReadField(); ext.valid();
           ext = ext_decoder.ReadField()) {
        if (!IsLengthDelimited(ext)) {
          continue;
        }
        switch (ext.id()) {
          case FrameworksBaseWinscopeExtensions::kWindowmanagerFieldNumber:
          case FrameworksBaseWinscopeExtensions::kWindowmanagerPatchFieldNumber:
            context->stats_tracker->IncrementStats(
                stats::winscope_windowmanager_protovm_state_bytes, state_bytes);
            return;
          case FrameworksNativeWinscopeExtensions::
              kSurfaceflingerLayersPatchFieldNumber:
            context->stats_tracker->IncrementStats(
                stats::winscope_sf_layers_protovm_state_bytes, state_bytes);
            return;
          case FrameworksBaseWinscopeExtensions::kViewcaptureFieldNumber:
            context->stats_tracker->IncrementStats(
                stats::winscope_viewcapture_protovm_state_bytes, state_bytes);
            return;
          default:
            break;
        }
      }
    }
  }
}

void RecordProtoVmsStats(TraceProcessorContext* context,
                         const protozero::Field& protovms_field) {
  if (!IsLengthDelimited(protovms_field)) {
    return;
  }
  protozero::ProtoDecoder protovms_decoder(protovms_field.as_bytes());
  for (protozero::Field inst_field = protovms_decoder.ReadField();
       inst_field.valid(); inst_field = protovms_decoder.ReadField()) {
    if (inst_field.id() != TracePacket::ProtoVms::kInstanceFieldNumber ||
        !IsLengthDelimited(inst_field)) {
      continue;
    }
    protozero::ProtoDecoder inst_decoder(inst_field.as_bytes());
    for (protozero::Field f = inst_decoder.ReadField(); f.valid();
         f = inst_decoder.ReadField()) {
      if (f.id() == TracePacket::ProtoVms::Instance::kStateFieldNumber &&
          IsLengthDelimited(f)) {
        RecordProtoVmStateBytes(context, f.as_bytes());
        break;
      }
    }
  }
}

// Attributes standalone `interned_data` packets carrying Winscope string
// interning extensions to their trace type when the outer TracePacket has no
// Winscope payload field.
void RecordWinscopeInternedDataStats(TraceProcessorContext* context,
                                     const protozero::Field& interned_field,
                                     int64_t raw_packet_bytes) {
  if (!IsLengthDelimited(interned_field)) {
    return;
  }
  bool has_protolog = false;
  bool has_viewcapture = false;
  int64_t protolog_inner_bytes = 0;
  int64_t viewcapture_inner_bytes = 0;
  protozero::ProtoDecoder interned_decoder(interned_field.as_bytes());
  for (protozero::Field sub = interned_decoder.ReadField(); sub.valid();
       sub = interned_decoder.ReadField()) {
    if (!IsLengthDelimited(sub)) {
      continue;
    }
    switch (sub.id()) {
      case FrameworksNativeInternedData::kProtologStringArgsFieldNumber:
      case FrameworksNativeInternedData::kProtologStacktraceFieldNumber:
        has_protolog = true;
        protolog_inner_bytes += static_cast<int64_t>(sub.size());
        break;
      case FrameworksBaseInternedData::kViewcapturePackageNameFieldNumber:
      case FrameworksBaseInternedData::kViewcaptureWindowNameFieldNumber:
      case FrameworksBaseInternedData::kViewcaptureViewIdFieldNumber:
      case FrameworksBaseInternedData::kViewcaptureClassNameFieldNumber:
      case FrameworksBaseInternedData::
          kViewcaptureContentDescriptionFieldNumber:
      case FrameworksBaseInternedData::kViewcaptureTextFieldNumber:
        has_viewcapture = true;
        viewcapture_inner_bytes += static_cast<int64_t>(sub.size());
        break;
      default:
        break;
    }
  }
  if (has_protolog && !has_viewcapture) {
    context->stats_tracker->IncrementStats(stats::winscope_protolog_packets);
    context->stats_tracker->IncrementStats(stats::winscope_protolog_bytes,
                                           raw_packet_bytes);
  } else if (has_viewcapture && !has_protolog) {
    context->stats_tracker->IncrementStats(stats::winscope_viewcapture_packets);
    context->stats_tracker->IncrementStats(stats::winscope_viewcapture_bytes,
                                           raw_packet_bytes);
  } else if (has_protolog && has_viewcapture) {
    // Mixed packet carrying both ProtoLog and ViewCapture interned data: avoid
    // double-counting raw_packet_bytes by charging each trace type only its
    // own inner payload bytes, and credit the single packet to the larger one.
    if (protolog_inner_bytes >= viewcapture_inner_bytes) {
      context->stats_tracker->IncrementStats(stats::winscope_protolog_packets);
    } else {
      context->stats_tracker->IncrementStats(
          stats::winscope_viewcapture_packets);
    }
    context->stats_tracker->IncrementStats(stats::winscope_protolog_bytes,
                                           protolog_inner_bytes);
    context->stats_tracker->IncrementStats(stats::winscope_viewcapture_bytes,
                                           viewcapture_inner_bytes);
  }
}

}  // namespace

WinscopeModule::WinscopeModule(ProtoImporterModuleContext* module_context,
                               TraceProcessorContext* context)
    : ProtoImporterModule(module_context),
      context_{context},
      args_parser_{*context->descriptor_pool_,
                   *context->storage->mutable_string_pool()},
      surfaceflinger_layers_parser_(&context_),
      surfaceflinger_transactions_parser_(context),
      shell_transitions_parser_(&context_),
      protolog_parser_(&context_),
      android_input_event_parser_(context),
      viewcapture_parser_(&context_),
      windowmanager_parser_(&context_) {
  RegisterForRawPackets();
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

void WinscopeModule::OnRawPacket(const SelectiveTracePacketDecoder& decoder,
                                 const TraceBlobView& packet) {
  auto* trace_processor_context = context_.trace_processor_context_;
  if (decoder.has_protovms()) {
    RecordProtoVmsStats(trace_processor_context, decoder.protovms_field());
    return;
  }
  if (decoder.has_track_event()) {
    return;
  }

  const int64_t raw_packet_bytes = static_cast<int64_t>(packet.length());
  for (protozero::Field field : decoder.unknown_fields()) {
    if (!IsLengthDelimited(field)) {
      continue;
    }
    switch (field.id()) {
      case FrameworksNativeWinscopeTracePacket::
          kSurfaceflingerLayersSnapshotFieldNumber:
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_sf_layers_packets);
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_sf_layers_bytes, raw_packet_bytes);
        return;
      case FrameworksNativeWinscopeTracePacket::
          kSurfaceflingerTransactionsFieldNumber:
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_sf_transactions_packets);
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_sf_transactions_bytes, raw_packet_bytes);
        return;
      case FrameworksBaseWinscopeTracePacket::kShellTransitionFieldNumber:
      case FrameworksBaseWinscopeTracePacket::kShellHandlerMappingsFieldNumber:
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_shell_transitions_packets);
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_shell_transitions_bytes, raw_packet_bytes);
        return;
      case FrameworksNativeWinscopeTracePacket::kProtologMessageFieldNumber:
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_protolog_packets);
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_protolog_bytes, raw_packet_bytes);
        return;
      case FrameworksNativeWinscopeTracePacket::
          kProtologViewerConfigFieldNumber:
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_protolog_view_config_packets);
        trace_processor_context->stats_tracker->IncrementStats(
            stats::winscope_protolog_view_config_bytes, raw_packet_bytes);
        return;
      case FrameworksNativeWinscopeTracePacket::
          kWinscopeExtensionsFieldNumber: {
        protozero::ProtoDecoder ext_decoder(field.as_bytes());
        for (auto ext_it = ext_decoder.ReadField(); ext_it.valid();
             ext_it = ext_decoder.ReadField()) {
          if (!IsLengthDelimited(ext_it)) {
            continue;
          }
          switch (ext_it.id()) {
            case FrameworksBaseWinscopeExtensions::
                kInputmethodClientsFieldNumber:
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_inputmethod_clients_packets);
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_inputmethod_clients_bytes, raw_packet_bytes);
              return;
            case FrameworksBaseWinscopeExtensions::
                kInputmethodServiceFieldNumber:
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_inputmethod_service_packets);
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_inputmethod_service_bytes, raw_packet_bytes);
              return;
            case FrameworksBaseWinscopeExtensions::
                kInputmethodManagerServiceFieldNumber:
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_inputmethod_manager_service_packets);
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_inputmethod_manager_service_bytes,
                  raw_packet_bytes);
              return;
            case FrameworksBaseWinscopeExtensions::kViewcaptureFieldNumber:
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_viewcapture_packets);
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_viewcapture_bytes, raw_packet_bytes);
              return;
            case FrameworksNativeWinscopeExtensions::
                kAndroidInputEventFieldNumber: {
              protozero::ProtoDecoder input_decoder(ext_it.as_bytes());
              for (auto in_it = input_decoder.ReadField(); in_it.valid();
                   in_it = input_decoder.ReadField()) {
                if (!IsLengthDelimited(in_it)) {
                  continue;
                }
                switch (in_it.id()) {
                  case AndroidInputEvent::kDispatcherMotionEventFieldNumber:
                  case AndroidInputEvent::
                      kDispatcherMotionEventRedactedFieldNumber:
                    trace_processor_context->stats_tracker->IncrementStats(
                        stats::winscope_input_motion_packets);
                    trace_processor_context->stats_tracker->IncrementStats(
                        stats::winscope_input_motion_bytes, raw_packet_bytes);
                    return;
                  case AndroidInputEvent::kDispatcherKeyEventFieldNumber:
                  case AndroidInputEvent::
                      kDispatcherKeyEventRedactedFieldNumber:
                    trace_processor_context->stats_tracker->IncrementStats(
                        stats::winscope_input_key_packets);
                    trace_processor_context->stats_tracker->IncrementStats(
                        stats::winscope_input_key_bytes, raw_packet_bytes);
                    return;
                  case AndroidInputEvent::
                      kDispatcherWindowDispatchEventFieldNumber:
                  case AndroidInputEvent::
                      kDispatcherWindowDispatchEventRedactedFieldNumber:
                    trace_processor_context->stats_tracker->IncrementStats(
                        stats::winscope_input_window_dispatch_packets);
                    trace_processor_context->stats_tracker->IncrementStats(
                        stats::winscope_input_window_dispatch_bytes,
                        raw_packet_bytes);
                    return;
                  default:
                    break;
                }
              }
              break;
            }
            case FrameworksBaseWinscopeExtensions::kWindowmanagerFieldNumber:
            case FrameworksBaseWinscopeExtensions::
                kWindowmanagerPatchFieldNumber:
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_windowmanager_packets);
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_windowmanager_bytes, raw_packet_bytes);
              return;
            case FrameworksNativeWinscopeExtensions::
                kSurfaceflingerLayersPatchFieldNumber:
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_sf_layers_packets);
              trace_processor_context->stats_tracker->IncrementStats(
                  stats::winscope_sf_layers_bytes, raw_packet_bytes);
              return;
            default:
              break;
          }
        }
        break;
      }
      default:
        break;
    }
  }

  if (decoder.has_interned_data()) {
    RecordWinscopeInternedDataStats(trace_processor_context,
                                    decoder.interned_data_field(),
                                    raw_packet_bytes);
  }
}

ModuleResult WinscopeModule::TokenizePacket(const TokenizePacketArgs& args) {
  switch (args.field.id()) {
    case FrameworksNativeWinscopeTracePacket::kProtologViewerConfigFieldNumber:
      protolog_parser_.ParseAndAddViewerConfigToMessageDecoder(
          args.field.Cast<
              FrameworksNativeWinscopeTracePacket::kProtologViewerConfig>());
      return ModuleResult::Handled();
  }

  return ModuleResult::Ignored();
}

void WinscopeModule::ParseField(const ParseFieldArgs& args) {
  std::optional<uint32_t> sequence_id;
  if (args.decoder.has_trusted_packet_sequence_id()) {
    sequence_id = args.decoder.trusted_packet_sequence_id();
  }
  switch (args.field.id()) {
    case FrameworksNativeWinscopeTracePacket::
        kSurfaceflingerLayersSnapshotFieldNumber:
      surfaceflinger_layers_parser_.Parse(
          args.ts,
          args.field.Cast<FrameworksNativeWinscopeTracePacket::
                              kSurfaceflingerLayersSnapshot>(),
          sequence_id);
      return;
    case FrameworksNativeWinscopeTracePacket::
        kSurfaceflingerTransactionsFieldNumber:
      surfaceflinger_transactions_parser_.Parse(
          args.ts, args.field.Cast<FrameworksNativeWinscopeTracePacket::
                                       kSurfaceflingerTransactions>());
      return;
    case FrameworksBaseWinscopeTracePacket::kShellTransitionFieldNumber:
      shell_transitions_parser_.ParseTransition(
          args.field
              .Cast<FrameworksBaseWinscopeTracePacket::kShellTransition>());
      return;
    case FrameworksBaseWinscopeTracePacket::kShellHandlerMappingsFieldNumber:
      shell_transitions_parser_.ParseHandlerMappings(
          args.field.Cast<
              FrameworksBaseWinscopeTracePacket::kShellHandlerMappings>());
      return;
    case FrameworksNativeWinscopeTracePacket::kProtologMessageFieldNumber:
      protolog_parser_.ParseProtoLogMessage(
          args.data.sequence_state.get(),
          args.field
              .Cast<FrameworksNativeWinscopeTracePacket::kProtologMessage>(),
          args.ts);
      return;
    case FrameworksNativeWinscopeTracePacket::kWinscopeExtensionsFieldNumber:
      ParseWinscopeExtensionsData(
          args.field
              .Cast<FrameworksNativeWinscopeTracePacket::kWinscopeExtensions>(),
          args.ts, args.data);
      return;
  }
}

void WinscopeModule::ParseWinscopeExtensionsData(protozero::ConstBytes blob,
                                                 int64_t timestamp,
                                                 const TracePacketData& data) {
  // WinscopeExtensions is purely a carrier of extension fields: walk them
  // all in wire order and dispatch on the field id.
  protozero::ProtoDecoder decoder(blob);
  for (protozero::Field f = decoder.ReadField(); f.valid();
       f = decoder.ReadField()) {
    TypedProtoField field(f);
    switch (field.id()) {
      case FrameworksBaseWinscopeExtensions::kInputmethodClientsFieldNumber:
        ParseInputMethodClientsData(
            timestamp,
            field
                .Cast<FrameworksBaseWinscopeExtensions::kInputmethodClients>());
        return;
      case FrameworksBaseWinscopeExtensions::
          kInputmethodManagerServiceFieldNumber:
        ParseInputMethodManagerServiceData(
            timestamp, field.Cast<FrameworksBaseWinscopeExtensions::
                                      kInputmethodManagerService>());
        return;
      case FrameworksBaseWinscopeExtensions::kInputmethodServiceFieldNumber:
        ParseInputMethodServiceData(
            timestamp,
            field
                .Cast<FrameworksBaseWinscopeExtensions::kInputmethodService>());
        return;
      case FrameworksBaseWinscopeExtensions::kViewcaptureFieldNumber:
        viewcapture_parser_.Parse(
            timestamp,
            field.Cast<FrameworksBaseWinscopeExtensions::kViewcapture>(),
            data.sequence_state.get());
        return;
      case FrameworksNativeWinscopeExtensions::kAndroidInputEventFieldNumber:
        android_input_event_parser_.ParseAndroidInputEvent(
            timestamp,
            field.Cast<
                FrameworksNativeWinscopeExtensions::kAndroidInputEvent>());
        return;
      case FrameworksBaseWinscopeExtensions::kWindowmanagerFieldNumber:
        windowmanager_parser_.Parse(
            timestamp,
            field.Cast<FrameworksBaseWinscopeExtensions::kWindowmanager>());
        return;
    }
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

  auto inserter = trace_processor_context->args_tracker->AddArgsTo(rowId);
  util::ProtoArgsEnv env(timestamp,
                         trace_processor_context->process_tracker.get());
  ArgsInserterSink writer(inserter);
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::InputMethodClientsTable::Name()),
                                nullptr /* parse all fields */, env, writer);
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

  auto inserter = trace_processor_context->args_tracker->AddArgsTo(rowId);
  util::ProtoArgsEnv env(timestamp,
                         trace_processor_context->process_tracker.get());
  ArgsInserterSink writer(inserter);
  base::Status status = args_parser_.ParseMessage(
      blob,
      *util::winscope_proto_mapping::GetProtoName(
          tables::InputMethodManagerServiceTable::Name()),
      nullptr /* parse all fields */, env, writer);
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

  auto inserter = trace_processor_context->args_tracker->AddArgsTo(rowId);
  util::ProtoArgsEnv env(timestamp,
                         trace_processor_context->process_tracker.get());
  ArgsInserterSink writer(inserter);
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::InputMethodServiceTable::Name()),
                                nullptr /* parse all fields */, env, writer);
  if (!status.ok()) {
    trace_processor_context->stats_tracker->IncrementStats(
        stats::winscope_inputmethod_service_parse_errors);
  }
}

void WinscopeModule::OnEventsFullyExtracted() {
  context_.shell_transitions_tracker_.Flush();
}

}  // namespace perfetto::trace_processor
