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

#include "src/trace_processor/importers/proto/chrome_events_module.h"

#include "perfetto/ext/base/string_writer.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;

ChromeEventsModule::ChromeEventsModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context),
      context_(context),
      raw_chrome_metadata_event_id_(
          context->storage->InternString("chrome_event.metadata")),
      raw_chrome_legacy_system_trace_event_id_(
          context->storage->InternString("chrome_event.legacy_system_trace")),
      raw_chrome_legacy_user_trace_event_id_(
          context->storage->InternString("chrome_event.legacy_user_trace")),
      data_name_id_(context->storage->InternString("data")) {
  RegisterForField(TracePacket::kChromeEventsFieldNumber);
}

ModuleResult ChromeEventsModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView*,
    int64_t,
    RefPtr<PacketSequenceStateGeneration>,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kChromeEventsFieldNumber: {
      ParseChromeEventsMetadata(decoder.chrome_events());
      return ModuleResult::Ignored();
    }
  }
  return ModuleResult::Ignored();
}

void ChromeEventsModule::ParseTracePacketData(
    const protos::pbzero::TracePacket::Decoder& decoder,
    int64_t ts,
    const TracePacketData&,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kChromeEventsFieldNumber:
      ParseChromeEvents(ts, decoder.chrome_events());
      return;
  }
}

void ChromeEventsModule::ParseChromeEventsMetadata(protozero::ConstBytes blob) {
  TraceStorage* storage = context_->storage.get();
  protos::pbzero::ChromeEventBundle::Decoder bundle(blob);
  if (!bundle.has_metadata())
    return;

  uint32_t bundle_index =
      context_->metadata_tracker->IncrementChromeMetadataBundleCount();

  // Insert into the metadata table during tokenization, so this metadata is
  // available before parsing begins. These metadata events are also added to
  // the raw table for JSON export at parsing time.
  for (auto it = bundle.metadata(); it; ++it) {
    protos::pbzero::ChromeMetadata::Decoder metadata(*it);
    Variadic value = Variadic::Null();
    if (metadata.has_string_value()) {
      value = Variadic::String(storage->InternString(metadata.string_value()));
    } else if (metadata.has_int_value()) {
      value = Variadic::Integer(metadata.int_value());
    } else if (metadata.has_bool_value()) {
      value = Variadic::Integer(metadata.bool_value());
    } else if (metadata.has_json_value()) {
      value = Variadic::Json(storage->InternString(metadata.json_value()));
    } else {
      context_->storage->IncrementStats(stats::empty_chrome_metadata);
      continue;
    }

    char buffer[2048];
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendString("cr-");
    // If we have data from multiple Chrome instances, append a suffix
    // to differentiate them.
    if (bundle_index > 1) {
      writer.AppendUnsignedInt(bundle_index);
      writer.AppendChar('-');
    }
    writer.AppendString(metadata.name());

    auto metadata_id = storage->InternString(writer.GetStringView());
    context_->metadata_tracker->SetDynamicMetadata(metadata_id, value);
  }
}

void ChromeEventsModule::ParseChromeEvents(int64_t ts,
                                           protozero::ConstBytes blob) {
  TraceStorage* storage = context_->storage.get();
  protos::pbzero::ChromeEventBundle::Decoder bundle(blob);
  ArgsTracker args(context_);
  if (bundle.has_metadata()) {
    tables::ChromeRawTable::Id id =
        storage->mutable_chrome_raw_table()
            ->Insert({ts, raw_chrome_metadata_event_id_, 0, 0})
            .id;

    // The legacy untyped metadata is proxied via a special event in the raw
    // table to JSON export. Entries into the metadata table are added during
    // tokenization by this module.
    for (auto it = bundle.metadata(); it; ++it) {
      protos::pbzero::ChromeMetadata::Decoder metadata(*it);
      Variadic value = Variadic::Null();
      if (metadata.has_string_value()) {
        value =
            Variadic::String(storage->InternString(metadata.string_value()));
      } else if (metadata.has_int_value()) {
        value = Variadic::Integer(metadata.int_value());
      } else if (metadata.has_bool_value()) {
        value = Variadic::Integer(metadata.bool_value());
      } else if (metadata.has_json_value()) {
        value = Variadic::Json(storage->InternString(metadata.json_value()));
      } else {
        context_->storage->IncrementStats(stats::empty_chrome_metadata);
        continue;
      }

      StringId name_id = storage->InternString(metadata.name());
      args.AddArgsTo(id).AddArg(name_id, value);
    }
  }

  if (bundle.has_legacy_ftrace_output()) {
    tables::ChromeRawTable::Id id =
        storage->mutable_chrome_raw_table()
            ->Insert({ts, raw_chrome_legacy_system_trace_event_id_, 0, 0})
            .id;

    std::string data;
    for (auto it = bundle.legacy_ftrace_output(); it; ++it) {
      data += (*it).ToStdString();
    }
    Variadic value =
        Variadic::String(storage->InternString(base::StringView(data)));
    args.AddArgsTo(id).AddArg(data_name_id_, value);
  }

  if (bundle.has_legacy_json_trace()) {
    for (auto it = bundle.legacy_json_trace(); it; ++it) {
      protos::pbzero::ChromeLegacyJsonTrace::Decoder legacy_trace(*it);
      if (legacy_trace.type() !=
          protos::pbzero::ChromeLegacyJsonTrace::USER_TRACE) {
        continue;
      }
      tables::ChromeRawTable::Id id =
          storage->mutable_chrome_raw_table()
              ->Insert({ts, raw_chrome_legacy_user_trace_event_id_, 0, 0})
              .id;
      Variadic value =
          Variadic::String(storage->InternString(legacy_trace.data()));
      args.AddArgsTo(id).AddArg(data_name_id_, value);
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
