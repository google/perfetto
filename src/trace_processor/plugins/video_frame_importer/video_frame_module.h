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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_VIDEO_FRAME_IMPORTER_VIDEO_FRAME_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_VIDEO_FRAME_IMPORTER_VIDEO_FRAME_MODULE_H_

#include <cstdint>
#include <unordered_map>

#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Parses two top-level TracePacket fields:
//   * video_frame       (field 133) -> AndroidVideoFramesTable rows. The
//     encoded payload is held zero-copy as a TraceBlobView in
//     TraceStorage::video_frame_au_data() and exposed to SQL via the
//     __INTRINSIC_VIDEO_FRAME_AU_DATA(id) scalar function.
//   * video_frame_error (field 134) -> one of seven kIndexed entries in
//     the global stats table (android_video_size_cap_hit / _codec_error /
//     _display_gone / _no_encoder / _display_not_found /
//     _encoder_setup_failed / _virtual_display_failed), keyed by
//     display_id. Same shape as ftrace's per-cpu stats: reviewers see
//     both "which stream" and "what kind of failure" without leaving the
//     stats table. Clean streams produce no rows.
//
// Frames are self-identifying: each carries a display_id. The codec_config
// packet additionally carries display_name and codec_string (RFC 6381) which
// we propagate to every row of the matching id so the UI can label the
// track and configure the decoder without parsing the SPS.
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
  void ParseVideoFrame(const protos::pbzero::TracePacket::Decoder& decoder,
                       int64_t ts,
                       const TracePacketData& data);
  void ParseVideoFrameError(
      const protos::pbzero::TracePacket::Decoder& decoder);

  struct StreamInfo {
    StringId display_name = kNullStringId;
    StringId codec_string = kNullStringId;
  };
  TraceProcessorContext* const context_;
  std::unordered_map<uint32_t, StreamInfo> stream_info_by_id_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_VIDEO_FRAME_IMPORTER_VIDEO_FRAME_MODULE_H_
