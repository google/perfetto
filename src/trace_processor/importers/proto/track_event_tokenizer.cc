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
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/importers/proto/track_event_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/range_of_interest.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
using protos::pbzero::CounterDescriptor;
}

TrackEventTokenizer::TrackEventTokenizer(TraceProcessorContext* context,
                                         TrackEventTracker* track_event_tracker)
    : context_(context),
      track_event_tracker_(track_event_tracker),
      counter_name_thread_time_id_(
          context_->storage->InternString("thread_time")),
      counter_name_thread_instruction_count_id_(
          context_->storage->InternString("thread_instruction_count")) {}

ModuleResult TrackEventTokenizer::TokenizeRangeOfInterestPacket(
    PacketSequenceState* /*state*/,
    const protos::pbzero::TracePacket::Decoder& packet,
    int64_t /*packet_timestamp*/) {
  protos::pbzero::TrackEventRangeOfInterest::Decoder range_of_interest(
      packet.track_event_range_of_interest());
  if (!range_of_interest.has_start_us()) {
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return ModuleResult::Handled();
  }
  track_event_tracker_->SetRangeOfInterestStartUs(range_of_interest.start_us());
  context_->metadata_tracker->SetMetadata(
      metadata::range_of_interest_start_us,
      Variadic::Integer(range_of_interest.start_us()));
  return ModuleResult::Handled();
}

ModuleResult TrackEventTokenizer::TokenizeTrackDescriptorPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet,
    int64_t packet_timestamp) {
  auto track_descriptor_field = packet.track_descriptor();
  protos::pbzero::TrackDescriptor::Decoder track(track_descriptor_field.data,
                                                 track_descriptor_field.size);

  if (!track.has_uuid()) {
    PERFETTO_ELOG("TrackDescriptor packet without uuid");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return ModuleResult::Handled();
  }

  StringId name_id = kNullStringId;
  if (track.has_name())
    name_id = context_->storage->InternString(track.name());

  if (packet.has_trusted_pid()) {
    context_->process_tracker->UpdateTrustedPid(
        static_cast<uint32_t>(packet.trusted_pid()), track.uuid());
  }

  if (track.has_thread()) {
    protos::pbzero::ThreadDescriptor::Decoder thread(track.thread());

    if (!thread.has_pid() || !thread.has_tid()) {
      PERFETTO_ELOG(
          "No pid or tid in ThreadDescriptor for track with uuid %" PRIu64,
          track.uuid());
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return ModuleResult::Handled();
    }

    if (state->IsIncrementalStateValid()) {
      TokenizeThreadDescriptor(state, thread);
    }

    track_event_tracker_->ReserveDescriptorThreadTrack(
        track.uuid(), track.parent_uuid(), name_id,
        static_cast<uint32_t>(thread.pid()),
        static_cast<uint32_t>(thread.tid()), packet_timestamp);
  } else if (track.has_process()) {
    protos::pbzero::ProcessDescriptor::Decoder process(track.process());

    if (!process.has_pid()) {
      PERFETTO_ELOG("No pid in ProcessDescriptor for track with uuid %" PRIu64,
                    track.uuid());
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return ModuleResult::Handled();
    }

    track_event_tracker_->ReserveDescriptorProcessTrack(
        track.uuid(), name_id, static_cast<uint32_t>(process.pid()),
        packet_timestamp);
  } else if (track.has_counter()) {
    protos::pbzero::CounterDescriptor::Decoder counter(track.counter());

    StringId category_id = kNullStringId;
    if (counter.has_categories()) {
      // TODO(eseckler): Support multi-category events in the table schema.
      std::string categories;
      for (auto it = counter.categories(); it; ++it) {
        if (!categories.empty())
          categories += ",";
        categories.append((*it).data, (*it).size);
      }
      if (!categories.empty()) {
        category_id =
            context_->storage->InternString(base::StringView(categories));
      }
    }

    // TODO(eseckler): Intern counter tracks for specific counter types like
    // thread time, so that the same counter can be referred to from tracks with
    // different uuids. (Chrome may emit thread time values on behalf of other
    // threads, in which case it has to use absolute values on a different
    // track_uuid. Right now these absolute values are imported onto a separate
    // counter track than the other thread's regular thread time values.)
    if (name_id.is_null()) {
      switch (counter.type()) {
        case CounterDescriptor::COUNTER_UNSPECIFIED:
          break;
        case CounterDescriptor::COUNTER_THREAD_TIME_NS:
          name_id = counter_name_thread_time_id_;
          break;
        case CounterDescriptor::COUNTER_THREAD_INSTRUCTION_COUNT:
          name_id = counter_name_thread_instruction_count_id_;
          break;
      }
    }

    track_event_tracker_->ReserveDescriptorCounterTrack(
        track.uuid(), track.parent_uuid(), name_id, category_id,
        counter.unit_multiplier(), counter.is_incremental(),
        packet.trusted_packet_sequence_id());
  } else {
    track_event_tracker_->ReserveDescriptorChildTrack(
        track.uuid(), track.parent_uuid(), name_id);
  }

  // Let ProtoTraceReader forward the packet to the parser.
  return ModuleResult::Ignored();
}

ModuleResult TrackEventTokenizer::TokenizeThreadDescriptorPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet) {
  if (PERFETTO_UNLIKELY(!packet.has_trusted_packet_sequence_id())) {
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

  protos::pbzero::ThreadDescriptor::Decoder thread(packet.thread_descriptor());
  TokenizeThreadDescriptor(state, thread);

  // Let ProtoTraceReader forward the packet to the parser.
  return ModuleResult::Ignored();
}

void TrackEventTokenizer::TokenizeThreadDescriptor(
    PacketSequenceState* state,
    const protos::pbzero::ThreadDescriptor::Decoder& thread) {
  // TODO(eseckler): Remove support for legacy thread descriptor-based default
  // tracks and delta timestamps.
  state->SetThreadDescriptor(thread.pid(), thread.tid(),
                             thread.reference_timestamp_us() * 1000,
                             thread.reference_thread_time_us() * 1000,
                             thread.reference_thread_instruction_count());
}

void TrackEventTokenizer::TokenizeTrackEventPacket(
    PacketSequenceState* state,
    const protos::pbzero::TracePacket::Decoder& packet,
    TraceBlobView* packet_blob,
    int64_t packet_timestamp) {
  if (PERFETTO_UNLIKELY(!packet.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("TrackEvent packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  auto field = packet.track_event();
  protos::pbzero::TrackEvent::Decoder event(field.data, field.size);

  protos::pbzero::TrackEventDefaults::Decoder* defaults =
      state->current_generation()->GetTrackEventDefaults();

  int64_t timestamp;
  TrackEventData data(std::move(*packet_blob), state->current_generation());

  // TODO(eseckler): Remove handling of timestamps relative to ThreadDescriptors
  // once all producers have switched to clock-domain timestamps (e.g.
  // TracePacket's timestamp).

  if (event.has_timestamp_delta_us()) {
    // Delta timestamps require a valid ThreadDescriptor packet since the last
    // packet loss.
    if (!state->track_event_timestamps_valid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return;
    }
    timestamp = state->IncrementAndGetTrackEventTimeNs(
        event.timestamp_delta_us() * 1000);

    // Legacy TrackEvent timestamp fields are in MONOTONIC domain. Adjust to
    // trace time if we have a clock snapshot.
    auto trace_ts = context_->clock_tracker->ToTraceTime(
        protos::pbzero::BUILTIN_CLOCK_MONOTONIC, timestamp);
    if (trace_ts.has_value())
      timestamp = trace_ts.value();
  } else if (int64_t ts_absolute_us = event.timestamp_absolute_us()) {
    // One-off absolute timestamps don't affect delta computation.
    timestamp = ts_absolute_us * 1000;

    // Legacy TrackEvent timestamp fields are in MONOTONIC domain. Adjust to
    // trace time if we have a clock snapshot.
    auto trace_ts = context_->clock_tracker->ToTraceTime(
        protos::pbzero::BUILTIN_CLOCK_MONOTONIC, timestamp);
    if (trace_ts.has_value())
      timestamp = trace_ts.value();
  } else if (packet.has_timestamp()) {
    timestamp = packet_timestamp;
  } else {
    PERFETTO_ELOG("TrackEvent without valid timestamp");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  if (event.has_thread_time_delta_us()) {
    // Delta timestamps require a valid ThreadDescriptor packet since the last
    // packet loss.
    if (!state->track_event_timestamps_valid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return;
    }
    data.thread_timestamp = state->IncrementAndGetTrackEventThreadTimeNs(
        event.thread_time_delta_us() * 1000);
  } else if (event.has_thread_time_absolute_us()) {
    // One-off absolute timestamps don't affect delta computation.
    data.thread_timestamp = event.thread_time_absolute_us() * 1000;
  }

  if (event.has_thread_instruction_count_delta()) {
    // Delta timestamps require a valid ThreadDescriptor packet since the last
    // packet loss.
    if (!state->track_event_timestamps_valid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return;
    }
    data.thread_instruction_count =
        state->IncrementAndGetTrackEventThreadInstructionCount(
            event.thread_instruction_count_delta());
  } else if (event.has_thread_instruction_count_absolute()) {
    // One-off absolute timestamps don't affect delta computation.
    data.thread_instruction_count = event.thread_instruction_count_absolute();
  }

  if (event.type() == protos::pbzero::TrackEvent::TYPE_COUNTER) {
    // Consider track_uuid from the packet and TrackEventDefaults.
    uint64_t track_uuid;
    if (event.has_track_uuid()) {
      track_uuid = event.track_uuid();
    } else if (defaults && defaults->has_track_uuid()) {
      track_uuid = defaults->track_uuid();
    } else {
      PERFETTO_DLOG(
          "Ignoring TrackEvent with counter_value but without track_uuid");
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return;
    }

    if (!event.has_counter_value() && !event.has_double_counter_value()) {
      PERFETTO_DLOG(
          "Ignoring TrackEvent with TYPE_COUNTER but without counter_value or "
          "double_counter_value for "
          "track_uuid %" PRIu64,
          track_uuid);
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return;
    }

    base::Optional<double> value;
    if (event.has_counter_value()) {
      value = track_event_tracker_->ConvertToAbsoluteCounterValue(
          track_uuid, packet.trusted_packet_sequence_id(),
          static_cast<double>(event.counter_value()));
    } else {
      value = track_event_tracker_->ConvertToAbsoluteCounterValue(
          track_uuid, packet.trusted_packet_sequence_id(),
          event.double_counter_value());
    }

    if (!value) {
      PERFETTO_DLOG("Ignoring TrackEvent with invalid track_uuid %" PRIu64,
                    track_uuid);
      context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
      return;
    }

    data.counter_value = *value;
  }

  size_t index = 0;
  const protozero::RepeatedFieldIterator<uint64_t> kEmptyIterator;
  auto result = AddExtraCounterValues(
      data, index, packet.trusted_packet_sequence_id(),
      event.extra_counter_values(), event.extra_counter_track_uuids(),
      defaults ? defaults->extra_counter_track_uuids() : kEmptyIterator);
  if (!result.ok()) {
    PERFETTO_DLOG("%s", result.c_message());
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }
  result = AddExtraCounterValues(
      data, index, packet.trusted_packet_sequence_id(),
      event.extra_double_counter_values(),
      event.extra_double_counter_track_uuids(),
      defaults ? defaults->extra_double_counter_track_uuids() : kEmptyIterator);
  if (!result.ok()) {
    PERFETTO_DLOG("%s", result.c_message());
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  context_->sorter->PushTrackEventPacket(timestamp, std::move(data));
}

template <typename T>
base::Status TrackEventTokenizer::AddExtraCounterValues(
    TrackEventData& data,
    size_t& index,
    uint32_t trusted_packet_sequence_id,
    protozero::RepeatedFieldIterator<T> value_it,
    protozero::RepeatedFieldIterator<uint64_t> packet_track_uuid_it,
    protozero::RepeatedFieldIterator<uint64_t> default_track_uuid_it) {
  if (!value_it)
    return base::OkStatus();

  // Consider extra_{double_,}counter_track_uuids from the packet and
  // TrackEventDefaults.
  protozero::RepeatedFieldIterator<uint64_t> track_uuid_it;
  if (packet_track_uuid_it) {
    track_uuid_it = packet_track_uuid_it;
  } else if (default_track_uuid_it) {
    track_uuid_it = default_track_uuid_it;
  } else {
    return base::Status(
        "Ignoring TrackEvent with extra_{double_,}counter_values but without "
        "extra_{double_,}counter_track_uuids");
  }

  for (; value_it; ++value_it, ++track_uuid_it, ++index) {
    if (!*track_uuid_it) {
      return base::Status(
          "Ignoring TrackEvent with more extra_{double_,}counter_values than "
          "extra_{double_,}counter_track_uuids");
    }
    if (index >= TrackEventData::kMaxNumExtraCounters) {
      return base::Status(
          "Ignoring TrackEvent with more extra_{double_,}counter_values than "
          "TrackEventData::kMaxNumExtraCounters");
    }
    base::Optional<double> abs_value =
        track_event_tracker_->ConvertToAbsoluteCounterValue(
            *track_uuid_it, trusted_packet_sequence_id,
            static_cast<double>(*value_it));
    if (!abs_value) {
      return base::Status(
          "Ignoring TrackEvent with invalid extra counter track");
    }
    data.extra_counter_values[index] = *abs_value;
  }
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
