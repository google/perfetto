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

#include "src/trace_processor/plugins/audio_frame_importer/audio_frame_module.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/trace_blob_view.h"

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/plugins/audio_frame_importer/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"

namespace perfetto::trace_processor {

using ::com::android::internal::pbzero::AudioFrame;
using ::com::android::internal::pbzero::AudioFrameError;
using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::perfetto::protos::pbzero::TracePacket;

AudioFrameModule::AudioFrameModule(ProtoImporterModuleContext* mc,
                                   TraceProcessorContext* context,
                                   tables::AndroidAudioFramesTable* table,
                                   std::vector<TraceBlobView>* au_data)
    : ProtoImporterModule(mc),
      context_(context),
      table_(table),
      au_data_(au_data) {
  RegisterForField(FrameworksBaseTracePacket::kAudioFrameFieldNumber);
  RegisterForField(FrameworksBaseTracePacket::kAudioFrameErrorFieldNumber);
}

AudioFrameModule::~AudioFrameModule() = default;

void AudioFrameModule::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                            int64_t ts,
                                            const TracePacketData& data,
                                            uint32_t field_id) {
  switch (field_id) {
    case FrameworksBaseTracePacket::kAudioFrameFieldNumber:
      ParseAudioFrame(decoder, ts, data);
      break;
    case FrameworksBaseTracePacket::kAudioFrameErrorFieldNumber:
      ParseAudioFrameError(decoder, ts);
      break;
    default:
      break;
  }
}

void AudioFrameModule::ParseAudioFrame(const TracePacket::Decoder& decoder,
                                       int64_t ts,
                                       const TracePacketData& data) {
  AudioFrame::Decoder frame(
      decoder
          .GetExtensionSlowly<
              FrameworksBaseTracePacket::kAudioFrameFieldNumber>()
          .as_bytes());

  const uint32_t stream_id = frame.has_stream_id() ? frame.stream_id() : 0u;

  // stream_name/codec_string/sample_rate/channels arrive on the codec_config
  // packet; cache them so later frames of the stream inherit them.
  StreamInfo& info = stream_info_by_id_[stream_id];
  if (frame.has_stream_name()) {
    info.stream_name =
        context_->storage->InternString(frame.stream_name().ToStdStringView());
  }
  if (frame.has_codec_string()) {
    info.codec_string =
        context_->storage->InternString(frame.codec_string().ToStdStringView());
  }
  if (frame.has_sample_rate()) {
    info.sample_rate = static_cast<int32_t>(frame.sample_rate());
  }
  if (frame.has_channels()) {
    info.channels = static_cast<int32_t>(frame.channels());
  }

  tables::AndroidAudioFramesTable::Row row;
  row.ts = ts;
  row.stream_id = static_cast<int32_t>(stream_id);
  if (info.stream_name != kNullStringId) {
    row.stream_name = info.stream_name;
  }
  if (info.codec_string != kNullStringId) {
    row.codec_string = info.codec_string;
  }
  if (info.sample_rate != 0) {
    row.sample_rate = info.sample_rate;
  }
  if (info.channels != 0) {
    row.channels = info.channels;
  }
  row.frame_number =
      frame.has_frame_number() ? static_cast<int64_t>(frame.frame_number()) : 0;

  // An AudioFrame is either a codec_config (is_config = 1) or one encoded
  // frame. Skip a frame with no payload: every row must have bytes so the
  // au_data side-vector stays parallel to the table, indexed by row id.
  protozero::ConstBytes payload = {};
  if (frame.has_au_data()) {
    payload = frame.au_data();
    row.codec = frame.has_codec() ? static_cast<int32_t>(frame.codec()) : 0;
    if (frame.has_pts_us()) {
      row.pts_us = static_cast<int64_t>(frame.pts_us());
    }
    // Every encoded frame has an amplitude; a missing peak means 0 (silence),
    // since a zero-valued field is dropped on the wire. Default it so the
    // waveform reads as a flat zero rather than a NULL gap.
    row.peak = frame.has_peak() ? static_cast<int32_t>(frame.peak()) : 0;
  } else if (frame.has_codec_config()) {
    payload = frame.codec_config();
    row.codec = frame.has_codec() ? static_cast<int32_t>(frame.codec()) : 0;
    row.is_config = 1;
  }
  if (payload.size == 0) {
    return;
  }

  // Parse-time per-stream cap: drop frames once a stream exceeds it. Report
  // the first drop per stream to the import logs and let later drops fall
  // through silently.
  if (info.emitted_bytes + static_cast<int64_t>(payload.size) >
      max_stream_size_bytes_) {
    if (!info.size_cap_hit) {
      info.size_cap_hit = true;
      context_->import_logs_tracker->RecordParserError(
          stats::android_audio_parse_size_cap_hit, ts,
          [this, stream_id](ArgsTracker::BoundInserter& inserter) {
            inserter.AddArg(context_->storage->InternString("stream_id"),
                            Variadic::UnsignedInteger(stream_id));
          });
    }
    return;
  }
  info.emitted_bytes += static_cast<int64_t>(payload.size);

  auto id = table_->Insert(row).id.value;
  // au_data is parallel to the table, indexed by row id.
  PERFETTO_DCHECK(id == au_data_->size());
  const TraceBlobView& packet = data.packet;
  const uint8_t* base = packet.blob()->data();
  PERFETTO_DCHECK(payload.data >= base &&
                  payload.data + payload.size <= base + packet.blob()->size());
  au_data_->emplace_back(packet.slice(payload.data, payload.size));
}

void AudioFrameModule::ParseAudioFrameError(const TracePacket::Decoder& decoder,
                                            int64_t ts) {
  AudioFrameError::Decoder err(
      decoder
          .GetExtensionSlowly<
              FrameworksBaseTracePacket::kAudioFrameErrorFieldNumber>()
          .as_bytes());
  if (!err.has_reason())
    return;
  const uint32_t stream_id = err.has_stream_id() ? err.stream_id() : 0u;
  size_t stat;
  switch (err.reason()) {
    case AudioFrameError::SIZE_CAP_HIT:
      stat = stats::android_audio_size_cap_hit;
      break;
    case AudioFrameError::CODEC_ERROR:
      stat = stats::android_audio_codec_error;
      break;
    case AudioFrameError::NO_ENCODER:
      stat = stats::android_audio_no_encoder;
      break;
    case AudioFrameError::TAP_FAILED:
      stat = stats::android_audio_tap_failed;
      break;
    default:
      return;  // REASON_UNKNOWN or future enumerator
  }
  context_->import_logs_tracker->RecordCollectionError(
      stat, ts, [this, stream_id](ArgsTracker::BoundInserter& inserter) {
        inserter.AddArg(context_->storage->InternString("stream_id"),
                        Variadic::UnsignedInteger(stream_id));
      });
}

}  // namespace perfetto::trace_processor
