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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_VIDEO_FRAME_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_VIDEO_FRAME_MODULE_H_

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Parses TracePacket.video_frame messages and stores JPEG image data
// in a dedicated blob vector on TraceStorage (not in the args table).
class VideoFrameModule : public ProtoImporterModule {
 public:
  VideoFrameModule(ProtoImporterModuleContext* module_context,
                   TraceProcessorContext* context);
  ~VideoFrameModule() override;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData& data,
                            uint32_t field_id) override;

 private:
  TraceProcessorContext* context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_VIDEO_FRAME_MODULE_H_
