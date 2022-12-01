/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/metadata_minimal_module.h"

#include "perfetto/ext/base/base64.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/chrome/chrome_benchmark_metadata.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_metadata.pbzero.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;

MetadataMinimalModule::MetadataMinimalModule(TraceProcessorContext* context)
    : context_(context) {
  RegisterForField(TracePacket::kChromeMetadataFieldNumber, context);
  RegisterForField(TracePacket::kChromeBenchmarkMetadataFieldNumber, context);
}

ModuleResult MetadataMinimalModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView*,
    int64_t,
    PacketSequenceState*,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kChromeMetadataFieldNumber: {
      ParseChromeMetadataPacket(decoder.chrome_metadata());
      return ModuleResult::Handled();
    }
    case TracePacket::kChromeBenchmarkMetadataFieldNumber: {
      ParseChromeBenchmarkMetadata(decoder.chrome_benchmark_metadata());
      return ModuleResult::Handled();
    }
  }
  return ModuleResult::Ignored();
}

void MetadataMinimalModule::ParseChromeBenchmarkMetadata(ConstBytes blob) {
  TraceStorage* storage = context_->storage.get();
  MetadataTracker* metadata = context_->metadata_tracker.get();

  protos::pbzero::ChromeBenchmarkMetadata::Decoder packet(blob.data, blob.size);
  if (packet.has_benchmark_name()) {
    auto benchmark_name_id = storage->InternString(packet.benchmark_name());
    metadata->SetMetadata(metadata::benchmark_name,
                          Variadic::String(benchmark_name_id));
  }
  if (packet.has_benchmark_description()) {
    auto benchmark_description_id =
        storage->InternString(packet.benchmark_description());
    metadata->SetMetadata(metadata::benchmark_description,
                          Variadic::String(benchmark_description_id));
  }
  if (packet.has_label()) {
    auto label_id = storage->InternString(packet.label());
    metadata->SetMetadata(metadata::benchmark_label,
                          Variadic::String(label_id));
  }
  if (packet.has_story_name()) {
    auto story_name_id = storage->InternString(packet.story_name());
    metadata->SetMetadata(metadata::benchmark_story_name,
                          Variadic::String(story_name_id));
  }
  for (auto it = packet.story_tags(); it; ++it) {
    auto story_tag_id = storage->InternString(*it);
    metadata->AppendMetadata(metadata::benchmark_story_tags,
                             Variadic::String(story_tag_id));
  }
  if (packet.has_benchmark_start_time_us()) {
    metadata->SetMetadata(metadata::benchmark_start_time_us,
                          Variadic::Integer(packet.benchmark_start_time_us()));
  }
  if (packet.has_story_run_time_us()) {
    metadata->SetMetadata(metadata::benchmark_story_run_time_us,
                          Variadic::Integer(packet.story_run_time_us()));
  }
  if (packet.has_story_run_index()) {
    metadata->SetMetadata(metadata::benchmark_story_run_index,
                          Variadic::Integer(packet.story_run_index()));
  }
  if (packet.has_had_failures()) {
    metadata->SetMetadata(metadata::benchmark_had_failures,
                          Variadic::Integer(packet.had_failures()));
  }
}

void MetadataMinimalModule::ParseChromeMetadataPacket(ConstBytes blob) {
  TraceStorage* storage = context_->storage.get();
  MetadataTracker* metadata = context_->metadata_tracker.get();

  // Typed chrome metadata proto. The untyped metadata is parsed below in
  // ParseChromeEvents().
  protos::pbzero::ChromeMetadataPacket::Decoder packet(blob.data, blob.size);

  if (packet.has_background_tracing_metadata()) {
    auto background_tracing_metadata = packet.background_tracing_metadata();
    std::string base64 = base::Base64Encode(background_tracing_metadata.data,
                                            background_tracing_metadata.size);
    metadata->SetDynamicMetadata(
        storage->InternString("cr-background_tracing_metadata"),
        Variadic::String(storage->InternString(base::StringView(base64))));
  }

  if (packet.has_chrome_version_code()) {
    metadata->SetDynamicMetadata(
        storage->InternString("cr-playstore_version_code"),
        Variadic::Integer(packet.chrome_version_code()));
  }
  if (packet.has_enabled_categories()) {
    auto categories_id = storage->InternString(packet.enabled_categories());
    metadata->SetDynamicMetadata(storage->InternString("cr-enabled_categories"),
                                 Variadic::String(categories_id));
  }
}

}  // namespace trace_processor
}  // namespace perfetto
