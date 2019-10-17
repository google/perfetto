/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_MODULE_H_

#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto {
namespace trace_processor {

class TrackEventModule : public ProtoImporterModuleBase</*IsEnabled=*/1> {
 public:
  explicit TrackEventModule(TraceProcessorContext* context)
      : ProtoImporterModuleBase(context) {}

  ModuleResult TokenizePacket(const protos::pbzero::TracePacket::Decoder&) {
    // TODO(eseckler): implement.
    return ModuleResult::Ignored();
  }

  ModuleResult ParsePacket(const protos::pbzero::TracePacket::Decoder&,
                           const TimestampedTracePiece&) {
    // TODO(eseckler): implement.
    return ModuleResult::Ignored();
  }
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_MODULE_H_
