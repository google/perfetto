/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/tracing_service_module.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/decompressor.h"
#include "src/trace_processor/util/descriptors.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/extension_descriptor.pbzero.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

using TracePacket = protos::pbzero::TracePacket;

TracingServiceModule::TracingServiceModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(TracePacket::kServiceEventFieldNumber);
  RegisterForField(TracePacket::kExtensionDescriptorFieldNumber);
}

TracingServiceModule::~TracingServiceModule() = default;

ModuleResult TracingServiceModule::TokenizePacket(
    const TokenizePacketArgs& args) {
  switch (args.field.id()) {
    case TracePacket::kServiceEventFieldNumber: {
      // TracingServiceImpl always stamps lifecycle events with
      // GetBootTimeNs(), so the timestamp as written is BOOTTIME regardless
      // of primary_trace_clock. Convert it explicitly from the decoder
      // rather than using |args.ts|. If the conversion fails (e.g. clock
      // snapshotting was disabled), keep the raw value, which is trace time
      // in that case.
      PERFETTO_DCHECK(args.decoder.has_timestamp());
      int64_t ts = static_cast<int64_t>(args.decoder.timestamp());
      uint32_t timestamp_clock_id =
          args.decoder.has_timestamp_clock_id()
              ? args.decoder.timestamp_clock_id()
              : protos::pbzero::BUILTIN_CLOCK_BOOTTIME;
      if (auto trace_ts = context_->clock_tracker->ToTraceTime(
              ClockId::Machine(timestamp_clock_id), ts, args.packet->offset(),
              /*suppress_errors=*/true)) {
        ts = *trace_ts;
      }
      return ParseServiceEvent(ts,
                               args.field.Cast<TracePacket::kServiceEvent>());
    }
    case TracePacket::kExtensionDescriptorFieldNumber:
      return ParseExtensionDescriptor(
          args.field.Cast<TracePacket::kExtensionDescriptor>());
    default:
      return ModuleResult::Ignored();
  }
}

base::Status TracingServiceModule::ParseServiceEvent(int64_t ts,
                                                     ConstBytes blob) {
  protos::pbzero::TracingServiceEvent::Decoder tse(blob);
  if (tse.tracing_started()) {
    context_->metadata_tracker->SetMetadata(metadata::tracing_started_ns,
                                            Variadic::Integer(ts));
  }
  if (tse.tracing_disabled()) {
    context_->metadata_tracker->SetMetadata(metadata::tracing_disabled_ns,
                                            Variadic::Integer(ts));
  }
  if (tse.all_data_sources_started()) {
    context_->metadata_tracker->SetMetadata(
        metadata::all_data_source_started_ns, Variadic::Integer(ts));
  }
  if (tse.all_data_sources_flushed()) {
    context_->metadata_tracker->AppendMetadata(
        metadata::all_data_source_flushed_ns, Variadic::Integer(ts));
    context_->sorter->NotifyFlushEvent();
  }
  if (tse.read_tracing_buffers_completed()) {
    context_->sorter->NotifyReadBufferEvent();
  }
  if (tse.has_slow_starting_data_sources()) {
    protos::pbzero::TracingServiceEvent::DataSources::Decoder msg(
        tse.slow_starting_data_sources());
    for (auto it = msg.data_source(); it; it++) {
      protos::pbzero::TracingServiceEvent::DataSources::DataSource::Decoder
          data_source(*it);
      std::string formatted = data_source.producer_name().ToStdString() + " " +
                              data_source.data_source_name().ToStdString();
      context_->metadata_tracker->AppendMetadata(
          metadata::slow_start_data_source,
          Variadic::String(
              context_->storage->InternString(base::StringView(formatted))));
    }
  }
  if (tse.has_clone_started()) {
    context_->stats_tracker->SetStats(stats::traced_clone_started_timestamp_ns,
                                      ts);
  }
  if (tse.has_buffer_cloned()) {
    context_->stats_tracker->SetIndexedStats(
        stats::traced_buf_clone_done_timestamp_ns,
        static_cast<int>(tse.buffer_cloned()), ts);
  }
  return base::OkStatus();
}

base::Status TracingServiceModule::ParseExtensionDescriptor(
    ConstBytes descriptor) {
  protos::pbzero::ExtensionDescriptor::Decoder decoder(descriptor.data,
                                                       descriptor.size);

  const uint8_t* data = nullptr;
  size_t size = 0;
  std::optional<util::DecompressedBuffer> decompressed;
  if (decoder.has_extension_set()) {
    auto extension = decoder.extension_set();
    data = extension.data;
    size = extension.size;
  } else if (decoder.has_extension_set_gzip()) {
    auto gzipped = decoder.extension_set_gzip();
    decompressed = util::DecompressToBuffer(util::CompressionType::kGzip,
                                            gzipped.data, gzipped.size);
    if (!decompressed || decompressed->size == 0) {
      return base::ErrStatus(
          "Failed to decompress gzipped extension descriptor (ERR:tp-corrupt)");
    }
    data = decompressed->data.get();
    size = decompressed->size;
  } else {
    return base::OkStatus();
  }

  return context_->descriptor_pool_->AddFromFileDescriptorSet(
      data, size,
      /*skip_prefixes*/ {},
      /*merge_existing_messages=*/true);
}

}  // namespace perfetto::trace_processor
