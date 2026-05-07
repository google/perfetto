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

#include "src/trace_processor/importers/proto/video_frame_module.h"

#include <cstdint>

#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/android/video_frame.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

using protos::pbzero::TracePacket;

VideoFrameModule::~VideoFrameModule() = default;

VideoFrameModule::VideoFrameModule(ProtoImporterModuleContext* module_context,
                                   TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(TracePacket::kVideoFrameFieldNumber);
}

void VideoFrameModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                            int64_t ts,
                                            const TracePacketData&,
                                            uint32_t field_id) {
  if (field_id != TracePacket::kVideoFrameFieldNumber) {
    return;
  }

  protos::pbzero::VideoFrame::Decoder frame(decoder.video_frame());

  // Insert metadata row.
  tables::AndroidVideoFramesTable::Row row;
  row.ts = ts;
  row.frame_number =
      frame.has_frame_number() ? static_cast<int64_t>(frame.frame_number()) : 0;
  if (frame.has_track_name()) {
    row.track_name = context_->storage->InternString(frame.track_name());
  }
  if (frame.has_track_id()) {
    row.track_id = frame.track_id();
  }

  auto* table = context_->storage->mutable_video_frames_table();
  uint32_t row_idx = table->Insert(row).row;

  // Store image data — prefer JPEG, fall back to WebP.
  protozero::ConstBytes img = {};
  if (frame.has_jpg_image()) {
    img = frame.jpg_image();
  } else if (frame.has_webp_image()) {
    img = frame.webp_image();
  }
  if (img.size > 0) {
    auto blob = TraceBlob::Allocate(img.size);
    memcpy(blob.data(), img.data, img.size);
    auto* blob_vec = context_->storage->mutable_video_frame_data();
    if (blob_vec->size() <= row_idx) {
      blob_vec->resize(row_idx + 1);
    }
    (*blob_vec)[row_idx] = TraceBlobView(std::move(blob), 0, img.size);
  }
}

}  // namespace perfetto::trace_processor
