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

#include "src/trace_processor/plugins/video_frame_importer/video_frame_module.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/trace_blob_view.h"

#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/android/video_frame.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

using protos::pbzero::TracePacket;
using protos::pbzero::VideoFrame;
using protos::pbzero::VideoFrameError;

namespace {

StringId InternProtoString(TraceProcessorContext* context,
                           protozero::ConstChars sv) {
  return context->storage->InternString(
      base::StringView(reinterpret_cast<const char*>(sv.data), sv.size));
}

}  // namespace

VideoFrameModule::VideoFrameModule(ProtoImporterModuleContext* mc,
                                   TraceProcessorContext* context)
    : ProtoImporterModule(mc), context_(context) {
  RegisterForField(TracePacket::kVideoFrameFieldNumber);
  RegisterForField(TracePacket::kVideoFrameErrorFieldNumber);
}

VideoFrameModule::~VideoFrameModule() = default;

void VideoFrameModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                            int64_t ts,
                                            const TracePacketData& data,
                                            uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kVideoFrameFieldNumber:
      ParseVideoFrame(decoder, ts, data);
      break;
    case TracePacket::kVideoFrameErrorFieldNumber:
      ParseVideoFrameError(decoder);
      break;
    default:
      break;
  }
}

void VideoFrameModule::ParseVideoFrame(const TracePacket::Decoder& decoder,
                                       int64_t ts,
                                       const TracePacketData& data) {
  VideoFrame::Decoder frame(decoder.video_frame());

  const uint32_t display_id = frame.has_display_id() ? frame.display_id() : 0u;

  // display_name + codec_string arrive on the codec_config packet; cache
  // them so later frames of the stream inherit them.
  StreamInfo& info = stream_info_by_id_[display_id];
  if (frame.has_display_name()) {
    info.display_name = InternProtoString(context_, frame.display_name());
  }
  if (frame.has_codec_string()) {
    info.codec_string = InternProtoString(context_, frame.codec_string());
  }

  tables::AndroidVideoFramesTable::Row row;
  row.ts = ts;
  row.display_id = static_cast<int32_t>(display_id);
  if (info.display_name != kNullStringId) {
    row.display_name = info.display_name;
  }
  if (info.codec_string != kNullStringId) {
    row.codec_string = info.codec_string;
  }
  row.frame_number =
      frame.has_frame_number() ? static_cast<int64_t>(frame.frame_number()) : 0;

  // A VideoFrame is either a codec_config (is_config = 1) or one access
  // unit. Skip a frame with no payload: every row must have bytes so the
  // au_data side-vector stays parallel to the table, indexed by row id.
  protozero::ConstBytes payload = {};
  if (frame.has_au_data()) {
    payload = frame.au_data();
    row.codec = frame.has_codec() ? static_cast<int32_t>(frame.codec()) : 0;
    row.is_key_frame = frame.is_key_frame() ? 1 : 0;
    if (frame.has_pts_us()) {
      row.pts_us = static_cast<int64_t>(frame.pts_us());
    }
  } else if (frame.has_codec_config()) {
    payload = frame.codec_config();
    row.codec = frame.has_codec() ? static_cast<int32_t>(frame.codec()) : 0;
    row.is_config = 1;
  }
  if (payload.size == 0) {
    return;
  }

  // Parse-time per-stream cap: drop frames once a stream exceeds it,
  // reporting once via the same stat as the producer's cap.
  if (info.emitted_bytes + static_cast<int64_t>(payload.size) >
      max_stream_size_bytes_) {
    if (!info.size_cap_hit) {
      info.size_cap_hit = true;
      context_->stats_tracker->IncrementIndexedStats(
          stats::android_video_size_cap_hit, static_cast<int>(display_id));
    }
    return;
  }
  info.emitted_bytes += static_cast<int64_t>(payload.size);

  auto id =
      context_->storage->mutable_video_frames_table()->Insert(row).id.value;
  // au_data side-vector is parallel to the table, indexed by row id.
  auto* blobs = context_->storage->mutable_video_frame_au_data();
  PERFETTO_DCHECK(id == blobs->size());
  const TraceBlobView& packet = data.packet;
  const uint8_t* base = packet.blob()->data();
  PERFETTO_DCHECK(payload.data >= base &&
                  payload.data + payload.size <= base + packet.blob()->size());
  blobs->emplace_back(packet.slice(payload.data, payload.size));
}

void VideoFrameModule::ParseVideoFrameError(
    const TracePacket::Decoder& decoder) {
  VideoFrameError::Decoder err(decoder.video_frame_error());
  if (!err.has_reason())
    return;
  const int idx = err.has_display_id() ? static_cast<int>(err.display_id()) : 0;
  // Each reason maps to its own kIndexed stat, keyed by display_id.
  size_t stat;
  switch (err.reason()) {
    case VideoFrameError::SIZE_CAP_HIT:
      stat = stats::android_video_size_cap_hit;
      break;
    case VideoFrameError::CODEC_ERROR:
      stat = stats::android_video_codec_error;
      break;
    case VideoFrameError::DISPLAY_GONE:
      stat = stats::android_video_display_gone;
      break;
    case VideoFrameError::NO_ENCODER:
      stat = stats::android_video_no_encoder;
      break;
    case VideoFrameError::DISPLAY_NOT_FOUND:
      stat = stats::android_video_display_not_found;
      break;
    case VideoFrameError::ENCODER_SETUP_FAILED:
      stat = stats::android_video_encoder_setup_failed;
      break;
    case VideoFrameError::VIRTUAL_DISPLAY_FAILED:
      stat = stats::android_video_virtual_display_failed;
      break;
    default:
      return;  // REASON_UNKNOWN or future enumerator
  }
  context_->stats_tracker->IncrementIndexedStats(stat, idx);
}

}  // namespace perfetto::trace_processor
