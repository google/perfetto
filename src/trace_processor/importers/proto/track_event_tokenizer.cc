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
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

TrackEventTokenizer::TrackEventTokenizer(TraceProcessorContext* context)
    : context_(context),
      process_name_ids_{{context_->storage->InternString("Unknown"),
                         context_->storage->InternString("Browser"),
                         context_->storage->InternString("Renderer"),
                         context_->storage->InternString("Utility"),
                         context_->storage->InternString("Zygote"),
                         context_->storage->InternString("SandboxHelper"),
                         context_->storage->InternString("Gpu"),
                         context_->storage->InternString("PpapiPlugin"),
                         context_->storage->InternString("PpapiBroker")}} {}

void TrackEventTokenizer::TokenizeTrackDescriptorPacket(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  auto track_descriptor_field = packet_decoder.track_descriptor();
  protos::pbzero::TrackDescriptor::Decoder track_descriptor_decoder(
      track_descriptor_field.data, track_descriptor_field.size);

  if (!track_descriptor_decoder.has_uuid()) {
    PERFETTO_ELOG("TrackDescriptor packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  base::Optional<UniquePid> upid;
  base::Optional<UniqueTid> utid;

  if (track_descriptor_decoder.has_process()) {
    auto process_descriptor_field = track_descriptor_decoder.process();
    protos::pbzero::ProcessDescriptor::Decoder process_descriptor_decoder(
        process_descriptor_field.data, process_descriptor_field.size);

    // TODO(eseckler): Also parse process name / type here.

    upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(process_descriptor_decoder.pid()));
  }

  if (track_descriptor_decoder.has_thread()) {
    auto thread_descriptor_field = track_descriptor_decoder.thread();
    protos::pbzero::ThreadDescriptor::Decoder thread_descriptor_decoder(
        thread_descriptor_field.data, thread_descriptor_field.size);

    TokenizeThreadDescriptor(thread_descriptor_decoder);
    utid = context_->process_tracker->UpdateThread(
        static_cast<uint32_t>(thread_descriptor_decoder.tid()),
        static_cast<uint32_t>(thread_descriptor_decoder.pid()));
    upid = *context_->storage->GetThread(*utid).upid;
  }

  StringId name_id =
      context_->storage->InternString(track_descriptor_decoder.name());

  context_->track_tracker->UpdateDescriptorTrack(
      track_descriptor_decoder.uuid(), name_id, upid, utid);
}

void TrackEventTokenizer::TokenizeProcessDescriptorPacket(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  protos::pbzero::ProcessDescriptor::Decoder process_descriptor_decoder(
      packet_decoder.process_descriptor());
  if (!process_descriptor_decoder.has_chrome_process_type())
    return;

  auto process_type = process_descriptor_decoder.chrome_process_type();
  size_t name_index =
      static_cast<size_t>(process_type) < process_name_ids_.size()
          ? static_cast<size_t>(process_type)
          : 0u;
  StringId name = process_name_ids_[name_index];

  // Don't override system-provided names.
  context_->process_tracker->SetProcessNameIfUnset(
      context_->process_tracker->GetOrCreateProcess(
          static_cast<uint32_t>(process_descriptor_decoder.pid())),
      name);
}

void TrackEventTokenizer::TokenizeThreadDescriptorPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("ThreadDescriptor packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  // TrackEvents will be ignored while incremental state is invalid. As a
  // consequence, we should also ignore any ThreadDescriptors received in this
  // state. Otherwise, any delta-encoded timestamps would be calculated
  // incorrectly once we move out of the packet loss state. Instead, wait until
  // the first subsequent descriptor after incremental state is cleared.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
    return;
  }

  auto thread_descriptor_field = packet_decoder.thread_descriptor();
  protos::pbzero::ThreadDescriptor::Decoder thread_descriptor_decoder(
      thread_descriptor_field.data, thread_descriptor_field.size);

  state->SetThreadDescriptor(
      thread_descriptor_decoder.pid(), thread_descriptor_decoder.tid(),
      thread_descriptor_decoder.reference_timestamp_us() * 1000,
      thread_descriptor_decoder.reference_thread_time_us() * 1000,
      thread_descriptor_decoder.reference_thread_instruction_count());

  TokenizeThreadDescriptor(thread_descriptor_decoder);
}

void TrackEventTokenizer::TokenizeThreadDescriptor(
    const protos::pbzero::ThreadDescriptor::Decoder&
        thread_descriptor_decoder) {
  base::StringView name;
  if (thread_descriptor_decoder.has_thread_name()) {
    name = thread_descriptor_decoder.thread_name();
  } else if (thread_descriptor_decoder.has_chrome_thread_type()) {
    using protos::pbzero::ThreadDescriptor;
    switch (thread_descriptor_decoder.chrome_thread_type()) {
      case ThreadDescriptor::CHROME_THREAD_MAIN:
        name = "CrProcessMain";
        break;
      case ThreadDescriptor::CHROME_THREAD_IO:
        name = "ChromeIOThread";
        break;
      case ThreadDescriptor::CHROME_THREAD_POOL_FG_WORKER:
        name = "ThreadPoolForegroundWorker&";
        break;
      case ThreadDescriptor::CHROME_THREAD_POOL_BG_WORKER:
        name = "ThreadPoolBackgroundWorker&";
        break;
      case ThreadDescriptor::CHROME_THREAD_POOL_FB_BLOCKING:
        name = "ThreadPoolSingleThreadForegroundBlocking&";
        break;
      case ThreadDescriptor::CHROME_THREAD_POOL_BG_BLOCKING:
        name = "ThreadPoolSingleThreadBackgroundBlocking&";
        break;
      case ThreadDescriptor::CHROME_THREAD_POOL_SERVICE:
        name = "ThreadPoolService";
        break;
      case ThreadDescriptor::CHROME_THREAD_COMPOSITOR_WORKER:
        name = "CompositorTileWorker&";
        break;
      case ThreadDescriptor::CHROME_THREAD_COMPOSITOR:
        name = "Compositor";
        break;
      case ThreadDescriptor::CHROME_THREAD_VIZ_COMPOSITOR:
        name = "VizCompositorThread";
        break;
      case ThreadDescriptor::CHROME_THREAD_SERVICE_WORKER:
        name = "ServiceWorkerThread&";
        break;
      case ThreadDescriptor::CHROME_THREAD_MEMORY_INFRA:
        name = "MemoryInfra";
        break;
      case ThreadDescriptor::CHROME_THREAD_SAMPLING_PROFILER:
        name = "StackSamplingProfiler";
        break;
      case ThreadDescriptor::CHROME_THREAD_UNSPECIFIED:
        name = "ChromeUnspecified";
        break;
    }
  }

  if (!name.empty()) {
    auto thread_name_id = context_->storage->InternString(name);
    ProcessTracker* procs = context_->process_tracker.get();
    // Don't override system-provided names.
    procs->SetThreadNameIfUnset(
        procs->UpdateThread(
            static_cast<uint32_t>(thread_descriptor_decoder.tid()),
            static_cast<uint32_t>(thread_descriptor_decoder.pid())),
        thread_name_id);
  }
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
  } else if (packet_decoder.timestamp()) {
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
