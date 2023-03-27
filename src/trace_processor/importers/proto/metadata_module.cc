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

#include "src/trace_processor/importers/proto/metadata_module.h"

#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/uuid.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/config.descriptor.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/protozero_to_text.h"

#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_uuid.pbzero.h"
#include "protos/perfetto/trace/trigger.pbzero.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;

MetadataModule::MetadataModule(TraceProcessorContext* context)
    : context_(context),
      producer_name_key_id_(context_->storage->InternString("producer_name")),
      trusted_producer_uid_key_id_(
          context_->storage->InternString("trusted_producer_uid")) {
  RegisterForField(TracePacket::kUiStateFieldNumber, context);
  RegisterForField(TracePacket::kTriggerFieldNumber, context);
  RegisterForField(TracePacket::kTraceUuidFieldNumber, context);
}

ModuleResult MetadataModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView*,
    int64_t,
    PacketSequenceState*,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kUiStateFieldNumber: {
      auto ui_state = decoder.ui_state();
      std::string base64 = base::Base64Encode(ui_state.data, ui_state.size);
      StringId id = context_->storage->InternString(base::StringView(base64));
      context_->metadata_tracker->SetMetadata(metadata::ui_state,
                                              Variadic::String(id));
      return ModuleResult::Handled();
    }
    case TracePacket::kTraceUuidFieldNumber: {
      // If both the TraceUuid packet and TraceConfig.trace_uuid_msb/lsb are
      // set, the former (which is emitted first) takes precedence. This is
      // because the UUID can change throughout the lifecycle of a tracing
      // session if gap-less snapshots are used. Each trace file has at most one
      // TraceUuid packet (i has if it comes from an older version of the
      // tracing service < v32)
      protos::pbzero::TraceUuid::Decoder uuid_packet(decoder.trace_uuid());
      if (uuid_packet.msb() != 0 || uuid_packet.lsb() != 0) {
        base::Uuid uuid(uuid_packet.lsb(), uuid_packet.msb());
        std::string str = uuid.ToPrettyString();
        StringId id = context_->storage->InternString(base::StringView(str));
        context_->metadata_tracker->SetMetadata(metadata::trace_uuid,
                                                Variadic::String(id));
        context_->uuid_found_in_trace = true;
      }
      return ModuleResult::Handled();
    }
  }
  return ModuleResult::Ignored();
}

void MetadataModule::ParseTracePacketData(
    const protos::pbzero::TracePacket::Decoder& decoder,
    int64_t ts,
    const TracePacketData&,
    uint32_t field_id) {
  if (field_id == TracePacket::kTriggerFieldNumber) {
    // We handle triggers at parse time rather at tokenization because
    // we add slices to tables which need to happen post-sorting.
    ParseTrigger(ts, decoder.trigger());
  }
}

void MetadataModule::ParseTrigger(int64_t ts, ConstBytes blob) {
  protos::pbzero::Trigger::Decoder trigger(blob.data, blob.size);
  StringId cat_id = kNullStringId;
  TrackId track_id = context_->track_tracker->GetOrCreateTriggerTrack();
  StringId name_id = context_->storage->InternString(trigger.trigger_name());
  context_->slice_tracker->Scoped(
      ts, track_id, cat_id, name_id,
      /* duration = */ 0,
      [&trigger, this](ArgsTracker::BoundInserter* args_table) {
        StringId producer_name =
            context_->storage->InternString(trigger.producer_name());
        if (!producer_name.is_null()) {
          args_table->AddArg(producer_name_key_id_,
                             Variadic::String(producer_name));
        }
        if (trigger.has_trusted_producer_uid()) {
          args_table->AddArg(trusted_producer_uid_key_id_,
                             Variadic::Integer(trigger.trusted_producer_uid()));
        }
      });
}

void MetadataModule::ParseTraceUuid(ConstBytes blob) {
  // If both the TraceUuid packet and TraceConfig.trace_uuid_msb/lsb are set,
  // the former (which is emitted first) takes precedence. This is because the
  // UUID can change throughout the lifecycle of a tracing session if gap-less
  // snapshots are used. Each trace file has at most one TraceUuid packet (i
  // has if it comes from an older version of the tracing service < v32)
  protos::pbzero::TraceUuid::Decoder uuid_packet(blob.data, blob.size);
  if (uuid_packet.msb() != 0 || uuid_packet.lsb() != 0) {
    base::Uuid uuid(uuid_packet.lsb(), uuid_packet.msb());
    std::string str = uuid.ToPrettyString();
    StringId id = context_->storage->InternString(base::StringView(str));
    context_->metadata_tracker->SetMetadata(metadata::trace_uuid,
                                            Variadic::String(id));
    context_->uuid_found_in_trace = true;
  }
}

void MetadataModule::ParseTraceConfig(
    const protos::pbzero::TraceConfig_Decoder& trace_config) {
  int64_t uuid_msb = trace_config.trace_uuid_msb();
  int64_t uuid_lsb = trace_config.trace_uuid_lsb();
  if (!context_->uuid_found_in_trace && (uuid_msb != 0 || uuid_lsb != 0)) {
    base::Uuid uuid(uuid_lsb, uuid_msb);
    std::string str = uuid.ToPrettyString();
    StringId id = context_->storage->InternString(base::StringView(str));
    context_->metadata_tracker->SetMetadata(metadata::trace_uuid,
                                            Variadic::String(id));
    context_->uuid_found_in_trace = true;
  }

  if (trace_config.has_unique_session_name()) {
    StringId id = context_->storage->InternString(
        base::StringView(trace_config.unique_session_name()));
    context_->metadata_tracker->SetMetadata(metadata::unique_session_name,
                                            Variadic::String(id));
  }

  DescriptorPool pool;
  pool.AddFromFileDescriptorSet(kConfigDescriptor.data(),
                                kConfigDescriptor.size());

  std::string text = protozero_to_text::ProtozeroToText(
      pool, ".perfetto.protos.TraceConfig",
      protozero::ConstBytes{
          trace_config.begin(),
          static_cast<uint32_t>(trace_config.end() - trace_config.begin())},
      protozero_to_text::kIncludeNewLines);
  StringId id = context_->storage->InternString(base::StringView(text));
  context_->metadata_tracker->SetMetadata(metadata::trace_config_pbtxt,
                                          Variadic::String(id));
}

}  // namespace trace_processor
}  // namespace perfetto
