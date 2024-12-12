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

#include "src/trace_processor/importers/proto/winscope/winscope_module.h"
#include "perfetto/ext/base/base64.h"
#include "protos/perfetto/trace/android/winscope_extensions.pbzero.h"
#include "protos/perfetto/trace/android/winscope_extensions_impl.pbzero.h"
#include "src/trace_processor/importers/proto/args_parser.h"
#include "src/trace_processor/importers/proto/winscope/viewcapture_args_parser.h"
#include "src/trace_processor/importers/proto/winscope/winscope.descriptor.h"
#include "src/trace_processor/util/winscope_proto_mapping.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;
using perfetto::protos::pbzero::WinscopeExtensionsImpl;

WinscopeModule::WinscopeModule(TraceProcessorContext* context)
    : context_{context},
      args_parser_{*context->descriptor_pool_.get()},
      surfaceflinger_layers_parser_(context),
      surfaceflinger_transactions_parser_(context),
      shell_transitions_parser_(context),
      protolog_parser_(context),
      android_input_event_parser_(context) {
  context->descriptor_pool_->AddFromFileDescriptorSet(
      kWinscopeDescriptor.data(), kWinscopeDescriptor.size());
  RegisterForField(TracePacket::kSurfaceflingerLayersSnapshotFieldNumber,
                   context);
  RegisterForField(TracePacket::kSurfaceflingerTransactionsFieldNumber,
                   context);
  RegisterForField(TracePacket::kShellTransitionFieldNumber, context);
  RegisterForField(TracePacket::kShellHandlerMappingsFieldNumber, context);
  RegisterForField(TracePacket::kProtologMessageFieldNumber, context);
  RegisterForField(TracePacket::kProtologViewerConfigFieldNumber, context);
  RegisterForField(TracePacket::kWinscopeExtensionsFieldNumber, context);
}

ModuleResult WinscopeModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView* /*packet*/,
    int64_t /*packet_timestamp*/,
    RefPtr<PacketSequenceStateGeneration> /*state*/,
    uint32_t field_id) {

  switch (field_id) {
    case TracePacket::kProtologViewerConfigFieldNumber:
      protolog_parser_.ParseAndAddViewerConfigToMessageDecoder(
          decoder.protolog_viewer_config());
      return ModuleResult::Handled();
  }

  return ModuleResult::Ignored();
}

void WinscopeModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                          int64_t timestamp,
                                          const TracePacketData& data,
                                          uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kSurfaceflingerLayersSnapshotFieldNumber:
      surfaceflinger_layers_parser_.Parse(
          timestamp, decoder.surfaceflinger_layers_snapshot());
      return;
    case TracePacket::kSurfaceflingerTransactionsFieldNumber:
      surfaceflinger_transactions_parser_.Parse(
          timestamp, decoder.surfaceflinger_transactions());
      return;
    case TracePacket::kShellTransitionFieldNumber:
      shell_transitions_parser_.ParseTransition(decoder.shell_transition());
      return;
    case TracePacket::kShellHandlerMappingsFieldNumber:
      shell_transitions_parser_.ParseHandlerMappings(
          decoder.shell_handler_mappings());
      return;
    case TracePacket::kProtologMessageFieldNumber:
      protolog_parser_.ParseProtoLogMessage(
          data.sequence_state.get(), decoder.protolog_message(), timestamp);
      return;
    case TracePacket::kWinscopeExtensionsFieldNumber:
      ParseWinscopeExtensionsData(decoder.winscope_extensions(), timestamp,
                                  data);
      return;
  }
}

void WinscopeModule::ParseWinscopeExtensionsData(protozero::ConstBytes blob,
                                                 int64_t timestamp,
                                                 const TracePacketData& data) {
  WinscopeExtensionsImpl::Decoder decoder(blob.data, blob.size);

  if (auto field =
          decoder.Get(WinscopeExtensionsImpl::kInputmethodClientsFieldNumber);
      field.valid()) {
    ParseInputMethodClientsData(timestamp, field.as_bytes());
  } else if (field = decoder.Get(
                 WinscopeExtensionsImpl::kInputmethodManagerServiceFieldNumber);
             field.valid()) {
    ParseInputMethodManagerServiceData(timestamp, field.as_bytes());
  } else if (field = decoder.Get(
                 WinscopeExtensionsImpl::kInputmethodServiceFieldNumber);
             field.valid()) {
    ParseInputMethodServiceData(timestamp, field.as_bytes());
  } else if (field =
                 decoder.Get(WinscopeExtensionsImpl::kViewcaptureFieldNumber);
             field.valid()) {
    ParseViewCaptureData(timestamp, field.as_bytes(),
                         data.sequence_state.get());
  } else if (field = decoder.Get(
                 WinscopeExtensionsImpl::kAndroidInputEventFieldNumber);
             field.valid()) {
    android_input_event_parser_.ParseAndroidInputEvent(timestamp,
                                                       field.as_bytes());
  } else if (field =
                 decoder.Get(WinscopeExtensionsImpl::kWindowmanagerFieldNumber);
             field.valid()) {
    ParseWindowManagerData(timestamp, field.as_bytes());
  }
}

void WinscopeModule::ParseInputMethodClientsData(int64_t timestamp,
                                                 protozero::ConstBytes blob) {
  tables::InputMethodClientsTable::Row row;
  row.ts = timestamp;
  row.base64_proto = context_->storage->mutable_string_pool()->InternString(
      base::StringView(base::Base64Encode(blob.data, blob.size)));
  row.base64_proto_id = row.base64_proto.raw_id();
  auto rowId =
      context_->storage->mutable_inputmethod_clients_table()->Insert(row).id;

  ArgsTracker tracker(context_);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *context_->storage.get());
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::InputMethodClientsTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::winscope_inputmethod_clients_parse_errors);
  }
}

void WinscopeModule::ParseInputMethodManagerServiceData(
    int64_t timestamp,
    protozero::ConstBytes blob) {
  tables::InputMethodManagerServiceTable::Row row;
  row.ts = timestamp;
  row.base64_proto = context_->storage->mutable_string_pool()->InternString(
      base::StringView(base::Base64Encode(blob.data, blob.size)));
  row.base64_proto_id = row.base64_proto.raw_id();
  auto rowId = context_->storage->mutable_inputmethod_manager_service_table()
                   ->Insert(row)
                   .id;

  ArgsTracker tracker(context_);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *context_->storage.get());
  base::Status status = args_parser_.ParseMessage(
      blob,
      *util::winscope_proto_mapping::GetProtoName(
          tables::InputMethodManagerServiceTable::Name()),
      nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::winscope_inputmethod_manager_service_parse_errors);
  }
}

void WinscopeModule::ParseInputMethodServiceData(int64_t timestamp,
                                                 protozero::ConstBytes blob) {
  tables::InputMethodServiceTable::Row row;
  row.ts = timestamp;
  row.base64_proto = context_->storage->mutable_string_pool()->InternString(
      base::StringView(base::Base64Encode(blob.data, blob.size)));
  row.base64_proto_id = row.base64_proto.raw_id();
  auto rowId =
      context_->storage->mutable_inputmethod_service_table()->Insert(row).id;

  ArgsTracker tracker(context_);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *context_->storage.get());
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::InputMethodServiceTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::winscope_inputmethod_service_parse_errors);
  }
}

void WinscopeModule::ParseViewCaptureData(
    int64_t timestamp,
    protozero::ConstBytes blob,
    PacketSequenceStateGeneration* sequence_state) {
  tables::ViewCaptureTable::Row row;
  row.ts = timestamp;
  row.base64_proto = context_->storage->mutable_string_pool()->InternString(
      base::StringView(base::Base64Encode(blob.data, blob.size)));
  row.base64_proto_id = row.base64_proto.raw_id();
  auto rowId = context_->storage->mutable_viewcapture_table()->Insert(row).id;

  ArgsTracker tracker(context_);
  auto inserter = tracker.AddArgsTo(rowId);
  ViewCaptureArgsParser writer(timestamp, inserter, *context_->storage.get(),
                               sequence_state);
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::ViewCaptureTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(stats::winscope_viewcapture_parse_errors);
  }
}

void WinscopeModule::ParseWindowManagerData(int64_t timestamp,
                                            protozero::ConstBytes blob) {
  tables::WindowManagerTable::Row row;
  row.ts = timestamp;
  row.base64_proto = context_->storage->mutable_string_pool()->InternString(
      base::StringView(base::Base64Encode(blob.data, blob.size)));
  row.base64_proto_id = row.base64_proto.raw_id();
  auto rowId = context_->storage->mutable_windowmanager_table()->Insert(row).id;

  ArgsTracker tracker(context_);
  auto inserter = tracker.AddArgsTo(rowId);
  ArgsParser writer(timestamp, inserter, *context_->storage.get());
  base::Status status =
      args_parser_.ParseMessage(blob,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::WindowManagerTable::Name()),
                                nullptr /* parse all fields */, writer);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::winscope_windowmanager_parse_errors);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
