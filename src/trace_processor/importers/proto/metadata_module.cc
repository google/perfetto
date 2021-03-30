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

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;

MetadataModule::MetadataModule(TraceProcessorContext* context)
    : context_(context) {
  RegisterForField(TracePacket::kUiStateFieldNumber, context);
}

ModuleResult MetadataModule::TokenizePacket(
    const protos::pbzero::TracePacket::Decoder& decoder,
    TraceBlobView*,
    int64_t,
    PacketSequenceState*,
    uint32_t field_id) {
  switch (field_id) {
    // TODO(lalitm): move other metadata field parsing here.
    case TracePacket::kUiStateFieldNumber: {
      auto ui_state = decoder.ui_state();
      std::string base64 = base::Base64Encode(ui_state.data, ui_state.size);
      StringId id = context_->storage->InternString(base::StringView(base64));
      context_->metadata_tracker->SetMetadata(metadata::ui_state,
                                              Variadic::String(id));
      return ModuleResult::Handled();
    }
  }
  return ModuleResult::Ignored();
}

}  // namespace trace_processor
}  // namespace perfetto
