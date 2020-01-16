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

#include "src/trace_processor/importers/proto/track_event_tokenizer.h"

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/trace_storage.h"
#include "src/trace_processor/track_tracker.h"

#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

TrackEventTokenizer::TrackEventTokenizer(TraceProcessorContext* context)
    : context_(context) {}

ModuleResult TrackEventTokenizer::TokenizeTrackDescriptorPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet_decoder,
    int64_t packet_timestamp) {
  auto track_descriptor_field = packet_decoder.track_descriptor();
  protos::pbzero::TrackDescriptor::Decoder track_descriptor_decoder(
      track_descriptor_field.data, track_descriptor_field.size);

  if (!track_descriptor_decoder.has_uuid()) {
    PERFETTO_ELOG("TrackDescriptor packet without uuid");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return ModuleResult::Handled();
  }

  if (track_descriptor_decoder.has_thread()) {
    auto thread_descriptor_field = track_descriptor_decoder.thread();
    protos::pbzero::ThreadDescriptor::Decoder thread_descriptor_decoder(
        thread_descriptor_field.data, thread_descriptor_field.size);

    if (!thread_descriptor_decoder.has_pid() ||
        !thread_descriptor_decoder.has_tid()) {
      PERFETTO_ELOG(
          "No pid or tid in ThreadDescriptor for track with uuid %" PRIu64,
          track_descriptor_decoder.uuid());
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return ModuleResult::Handled();
    }

    if (state->IsIncrementalStateValid()) {
      TokenizeThreadDescriptor(state, thread_descriptor_decoder);
    }

    context_->track_tracker->ReserveDescriptorThreadTrack(
        track_descriptor_decoder.uuid(), track_descriptor_decoder.parent_uuid(),
        static_cast<uint32_t>(thread_descriptor_decoder.pid()),
        static_cast<uint32_t>(thread_descriptor_decoder.tid()),
        packet_timestamp);
  } else if (track_descriptor_decoder.has_process()) {
    auto process_descriptor_field = track_descriptor_decoder.process();
    protos::pbzero::ProcessDescriptor::Decoder process_descriptor_decoder(
        process_descriptor_field.data, process_descriptor_field.size);

    if (!process_descriptor_decoder.has_pid()) {
      PERFETTO_ELOG("No pid in ProcessDescriptor for track with uuid %" PRIu64,
                    track_descriptor_decoder.uuid());
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return ModuleResult::Handled();
    }

    context_->track_tracker->ReserveDescriptorProcessTrack(
        track_descriptor_decoder.uuid(),
        static_cast<uint32_t>(process_descriptor_decoder.pid()),
        packet_timestamp);
  } else {
    context_->track_tracker->ReserveDescriptorChildTrack(
        track_descriptor_decoder.uuid(),
        track_descriptor_decoder.parent_uuid());
  }

  // Let ProtoTraceTokenizer forward the packet to the parser.
  return ModuleResult::Ignored();
}

ModuleResult TrackEventTokenizer::TokenizeThreadDescriptorPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("ThreadDescriptor packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return ModuleResult::Handled();
  }

  // TrackEvents will be ignored while incremental state is invalid. As a
  // consequence, we should also ignore any ThreadDescriptors received in this
  // state. Otherwise, any delta-encoded timestamps would be calculated
  // incorrectly once we move out of the packet loss state. Instead, wait until
  // the first subsequent descriptor after incremental state is cleared.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
    return ModuleResult::Handled();
  }

  auto thread_descriptor_field = packet_decoder.thread_descriptor();
  protos::pbzero::ThreadDescriptor::Decoder thread_descriptor_decoder(
      thread_descriptor_field.data, thread_descriptor_field.size);
  TokenizeThreadDescriptor(state, thread_descriptor_decoder);

  // Let ProtoTraceTokenizer forward the packet to the parser.
  return ModuleResult::Ignored();
}

void TrackEventTokenizer::TokenizeThreadDescriptor(
    PacketSequenceState* state,
    const protos::pbzero::ThreadDescriptor::Decoder&
        thread_descriptor_decoder) {
  // TODO(eseckler): Remove support for legacy thread descriptor-based default
  // tracks and delta timestamps.
  state->SetThreadDescriptor(
      thread_descriptor_decoder.pid(), thread_descriptor_decoder.tid(),
      thread_descriptor_decoder.reference_timestamp_us() * 1000,
      thread_descriptor_decoder.reference_thread_time_us() * 1000,
      thread_descriptor_decoder.reference_thread_instruction_count());
}

void TrackEventTokenizer::TokenizeTrackEventPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet_decoder,
    TraceBlobView* packet,
    int64_t packet_timestamp) {
  constexpr auto kTimestampDeltaUsFieldNumber =
      protos::pbzero::TrackEvent::kTimestampDeltaUsFieldNumber;
  constexpr auto kTimestampAbsoluteUsFieldNumber =
      protos::pbzero::TrackEvent::kTimestampAbsoluteUsFieldNumber;
  constexpr auto kThreadTimeDeltaUsFieldNumber =
      protos::pbzero::TrackEvent::kThreadTimeDeltaUsFieldNumber;
  constexpr auto kThreadTimeAbsoluteUsFieldNumber =
      protos::pbzero::TrackEvent::kThreadTimeAbsoluteUsFieldNumber;
  constexpr auto kThreadInstructionCountDeltaFieldNumber =
      protos::pbzero::TrackEvent::kThreadInstructionCountDeltaFieldNumber;
  constexpr auto kThreadInstructionCountAbsoluteFieldNumber =
      protos::pbzero::TrackEvent::kThreadInstructionCountAbsoluteFieldNumber;

  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("TrackEvent packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  // TODO(eseckler): For now, TrackEvents can only be parsed correctly while
  // incremental state for their sequence is valid, because chromium doesn't set
  // SEQ_NEEDS_INCREMENTAL_STATE yet. Remove this once it does.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
    return;
  }

  auto field = packet_decoder.track_event();
  protozero::ProtoDecoder event_decoder(field.data, field.size);

  int64_t timestamp;
  int64_t thread_timestamp = 0;
  int64_t thread_instructions = 0;

  // TODO(eseckler): Remove handling of timestamps relative to ThreadDescriptors
  // once all producers have switched to clock-domain timestamps (e.g.
  // TracePacket's timestamp).

  if (auto ts_delta_field =
          event_decoder.FindField(kTimestampDeltaUsFieldNumber)) {
    // Delta timestamps require a valid ThreadDescriptor packet since the last
    // packet loss.
    if (!state->track_event_timestamps_valid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return;
    }
    timestamp = state->IncrementAndGetTrackEventTimeNs(
        ts_delta_field.as_int64() * 1000);

    // Legacy TrackEvent timestamp fields are in MONOTONIC domain. Adjust to
    // trace time if we have a clock snapshot.
    auto trace_ts = context_->clock_tracker->ToTraceTime(
        protos::pbzero::ClockSnapshot::Clock::MONOTONIC, timestamp);
    if (trace_ts.has_value())
      timestamp = trace_ts.value();
  } else if (int64_t ts_absolute_us =
                 event_decoder.FindField(kTimestampAbsoluteUsFieldNumber)
                     .as_int64()) {
    // One-off absolute timestamps don't affect delta computation.
    timestamp = ts_absolute_us * 1000;

    // Legacy TrackEvent timestamp fields are in MONOTONIC domain. Adjust to
    // trace time if we have a clock snapshot.
    auto trace_ts = context_->clock_tracker->ToTraceTime(
        protos::pbzero::ClockSnapshot::Clock::MONOTONIC, timestamp);
    if (trace_ts.has_value())
      timestamp = trace_ts.value();
  } else if (packet_decoder.has_timestamp()) {
    timestamp = packet_timestamp;
  } else {
    PERFETTO_ELOG("TrackEvent without valid timestamp");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  if (auto tt_delta_field =
          event_decoder.FindField(kThreadTimeDeltaUsFieldNumber)) {
    // Delta timestamps require a valid ThreadDescriptor packet since the last
    // packet loss.
    if (!state->track_event_timestamps_valid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return;
    }
    thread_timestamp = state->IncrementAndGetTrackEventThreadTimeNs(
        tt_delta_field.as_int64() * 1000);
  } else if (auto tt_absolute_field =
                 event_decoder.FindField(kThreadTimeAbsoluteUsFieldNumber)) {
    // One-off absolute timestamps don't affect delta computation.
    thread_timestamp = tt_absolute_field.as_int64() * 1000;
  }

  if (auto ti_delta_field =
          event_decoder.FindField(kThreadInstructionCountDeltaFieldNumber)) {
    // Delta timestamps require a valid ThreadDescriptor packet since the last
    // packet loss.
    if (!state->track_event_timestamps_valid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return;
    }
    thread_instructions =
        state->IncrementAndGetTrackEventThreadInstructionCount(
            ti_delta_field.as_int64());
  } else if (auto ti_absolute_field = event_decoder.FindField(
                 kThreadInstructionCountAbsoluteFieldNumber)) {
    // One-off absolute timestamps don't affect delta computation.
    thread_instructions = ti_absolute_field.as_int64();
  }

  context_->sorter->PushTrackEventPacket(timestamp, thread_timestamp,
                                         thread_instructions, state,
                                         std::move(*packet));
}

}  // namespace trace_processor
}  // namespace perfetto
