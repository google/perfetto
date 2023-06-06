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

namespace perfetto {
namespace trace_processor {

using perfetto::protos::pbzero::TracePacket;

WinscopeModule::WinscopeModule(TraceProcessorContext* context)
    : surfaceflinger_layers_parser_(context),
      surfaceflinger_transactions_parser_(context) {
  RegisterForField(TracePacket::kSurfaceflingerLayersSnapshotFieldNumber,
                   context);
  RegisterForField(TracePacket::kSurfaceflingerTransactionsFieldNumber,
                   context);
}

void WinscopeModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                          int64_t timestamp,
                                          const TracePacketData&,
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
  }
}

}  // namespace trace_processor
}  // namespace perfetto
