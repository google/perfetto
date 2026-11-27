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

#include "src/trace_processor/plugins/gpu_track_descriptor/gpu_track_descriptor.h"

#include <cstdint>
#include <memory>

#include "perfetto/base/compiler.h"
#include "protos/perfetto/trace/gpu/gpu_track_event.pbzero.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/importers/common/gpu_tracker.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/importers/proto/typed_proto_field.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::gpu_track_descriptor {
namespace {

using protos::pbzero::GpuTrackDescriptor;
using protos::pbzero::GpuTrackDescriptorExtension;

// Parses the GpuTrackDescriptor extension of TrackDescriptors into dimensions
// of their tracks.
class GpuTrackDescriptorParser : public TrackEventExtensionParser {
 public:
  GpuTrackDescriptorParser(TrackEventExtensionParserContext* parser_context,
                           TraceProcessorContext* context)
      : TrackEventExtensionParser(parser_context),
        trace_context_(context),
        ugpu_key_(context->storage->InternString("ugpu")),
        gpu_key_(context->storage->InternString("gpu")) {
    RegisterTrackDescriptorExtension(
        GpuTrackDescriptorExtension::kGpuTrackFieldNumber);
  }

  void OnTrackDescriptorField(
      const TypedProtoField& field,
      TrackDescriptorFieldContext* descriptor) override {
    GpuTrackDescriptor::Decoder gpu(
        field.Cast<GpuTrackDescriptorExtension::kGpuTrack>());
    if (gpu.has_gpu_id()) {
      if (gpu.gpu_id() < 0) {
        descriptor->errors.push_back(stats::track_descriptor_invalid_gpu_id);
      } else {
        // The well-known gpu dimension, with the same args as the tracks of
        // GPU data sources, so that they can be joined with the gpu table and
        // with each other.
        auto gpu_id = static_cast<uint32_t>(gpu.gpu_id());
        auto ugpu = trace_context_->gpu_tracker->GetOrCreateGpu(gpu_id);
        descriptor->dimensions.push_back({ugpu_key_, ugpu.value});
        descriptor->dimensions.push_back({gpu_key_, gpu_id});
      }
    }
  }

 private:
  TraceProcessorContext* const trace_context_;
  const StringId ugpu_key_;
  const StringId gpu_key_;
};

class GpuTrackDescriptorPlugin : public Plugin<GpuTrackDescriptorPlugin> {
 public:
  ~GpuTrackDescriptorPlugin() override;

  void RegisterTrackEventExtensions(
      TrackEventExtensionParserContext* ctx,
      TraceProcessorContext* trace_context) override {
    ctx->parsers.emplace_back(
        std::make_unique<GpuTrackDescriptorParser>(ctx, trace_context));
  }
};

GpuTrackDescriptorPlugin::~GpuTrackDescriptorPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<GpuTrackDescriptorPlugin>();
      },
      GpuTrackDescriptorPlugin::kPluginId,
      GpuTrackDescriptorPlugin::kDepIds.data(),
      GpuTrackDescriptorPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::gpu_track_descriptor
