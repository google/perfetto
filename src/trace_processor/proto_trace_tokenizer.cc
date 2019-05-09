/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/proto_trace_tokenizer.h"

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/trace_storage.h"

#include "perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/interned_data/interned_data.pbzero.h"
#include "perfetto/trace/trace.pbzero.h"
#include "perfetto/trace/track_event/task_execution.pbzero.h"
#include "perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

using protozero::ProtoDecoder;
using protozero::proto_utils::MakeTagLengthDelimited;
using protozero::proto_utils::MakeTagVarInt;
using protozero::proto_utils::ParseVarInt;

namespace {

template <typename MessageType>
void InternMessage(TraceProcessorContext* context,
                   ProtoIncrementalState::PacketSequenceState* state,
                   TraceBlobView message) {
  constexpr auto kIidFieldNumber = MessageType::kIidFieldNumber;

  uint32_t iid = 0;
  auto message_start = message.data();
  auto message_size = message.length();
  protozero::ProtoDecoder decoder(message_start, message_size);

  auto field = decoder.FindField(kIidFieldNumber);
  if (PERFETTO_UNLIKELY(!field)) {
    PERFETTO_ELOG("Interned message without interning_id");
    context->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }
  iid = field.as_uint32();

  auto res = state->GetInternedDataMap<MessageType>()->emplace(
      iid,
      ProtoIncrementalState::InternedDataView<MessageType>(std::move(message)));
  // If a message with this ID is already interned, its data should not have
  // changed (this is forbidden by the InternedData proto).
  // TODO(eseckler): This DCHECK assumes that the message is encoded the
  // same way whenever it is re-emitted.
  PERFETTO_DCHECK(res.second ||
                  (res.first->second.message.length() == message_size &&
                   memcmp(res.first->second.message.data(), message_start,
                          message_size) == 0));
}

}  // namespace

// static
TraceType ProtoTraceTokenizer::GuessProtoTraceType(const uint8_t* data,
                                                   size_t size) {
  // Scan at most the first 128MB for a track event packet.
  constexpr size_t kMaxScanSize = 128 * 1024 * 1024;
  protos::pbzero::Trace::Decoder decoder(data, std::min(size, kMaxScanSize));
  if (!decoder.has_packet())
    return TraceType::kUnknownTraceType;
  for (auto it = decoder.packet(); it; ++it) {
    ProtoDecoder packet_decoder(it->data(), it->size());
    if (PERFETTO_UNLIKELY(packet_decoder.FindField(
            protos::pbzero::TracePacket::kTrackEventFieldNumber))) {
      return TraceType::kProtoWithTrackEventsTraceType;
    }
  }
  return TraceType::kProtoTraceType;
}

ProtoTraceTokenizer::ProtoTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}
ProtoTraceTokenizer::~ProtoTraceTokenizer() = default;

bool ProtoTraceTokenizer::Parse(std::unique_ptr<uint8_t[]> owned_buf,
                                size_t size) {
  uint8_t* data = &owned_buf[0];
  if (!partial_buf_.empty()) {
    // It takes ~5 bytes for a proto preamble + the varint size.
    const size_t kHeaderBytes = 5;
    if (PERFETTO_UNLIKELY(partial_buf_.size() < kHeaderBytes)) {
      size_t missing_len = std::min(kHeaderBytes - partial_buf_.size(), size);
      partial_buf_.insert(partial_buf_.end(), &data[0], &data[missing_len]);
      if (partial_buf_.size() < kHeaderBytes)
        return true;
      data += missing_len;
      size -= missing_len;
    }

    // At this point we have enough data in |partial_buf_| to read at least the
    // field header and know the size of the next TracePacket.
    constexpr uint8_t kTracePacketTag =
        MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
    const uint8_t* pos = &partial_buf_[0];
    uint8_t proto_field_tag = *pos;
    uint64_t field_size = 0;
    const uint8_t* next = ParseVarInt(++pos, &*partial_buf_.end(), &field_size);
    bool parse_failed = next == pos;
    pos = next;
    if (proto_field_tag != kTracePacketTag || field_size == 0 || parse_failed) {
      PERFETTO_ELOG("Failed parsing a TracePacket from the partial buffer");
      return false;  // Unrecoverable error, stop parsing.
    }

    // At this point we know how big the TracePacket is.
    size_t hdr_size = static_cast<size_t>(pos - &partial_buf_[0]);
    size_t size_incl_header = static_cast<size_t>(field_size + hdr_size);
    PERFETTO_DCHECK(size_incl_header > partial_buf_.size());

    // There is a good chance that between the |partial_buf_| and the new |data|
    // of the current call we have enough bytes to parse a TracePacket.
    if (partial_buf_.size() + size >= size_incl_header) {
      // Create a new buffer for the whole TracePacket and copy into that:
      // 1) The beginning of the TracePacket (including the proto header) from
      //    the partial buffer.
      // 2) The rest of the TracePacket from the current |data| buffer (note
      //    that we might have consumed already a few bytes form |data| earlier
      //    in this function, hence we need to keep |off| into account).
      std::unique_ptr<uint8_t[]> buf(new uint8_t[size_incl_header]);
      memcpy(&buf[0], partial_buf_.data(), partial_buf_.size());
      // |size_missing| is the number of bytes for the rest of the TracePacket
      // in |data|.
      size_t size_missing = size_incl_header - partial_buf_.size();
      memcpy(&buf[partial_buf_.size()], &data[0], size_missing);
      data += size_missing;
      size -= size_missing;
      partial_buf_.clear();
      uint8_t* buf_start = &buf[0];  // Note that buf is std::moved below.
      bool success = ParseInternal(std::move(buf), buf_start, size_incl_header);
      if (PERFETTO_UNLIKELY(!success)) {
        PERFETTO_ELOG(
            "Failed to parse trace.  Check if the trace is corrupted.");
        return false;
      }
    } else {
      partial_buf_.insert(partial_buf_.end(), data, &data[size]);
      return true;
    }
  }
  bool success = ParseInternal(std::move(owned_buf), data, size);
  if (!success)
    PERFETTO_ELOG("Failed to parse trace. Check if the trace is corrupted.");
  return success;
}

bool ProtoTraceTokenizer::ParseInternal(std::unique_ptr<uint8_t[]> owned_buf,
                                        uint8_t* data,
                                        size_t size) {
  PERFETTO_DCHECK(data >= &owned_buf[0]);
  const uint8_t* start = &owned_buf[0];
  const size_t data_off = static_cast<size_t>(data - start);
  TraceBlobView whole_buf(std::move(owned_buf), data_off, size);

  protos::pbzero::Trace::Decoder decoder(data, size);
  for (auto it = decoder.packet(); it; ++it) {
    size_t field_offset = whole_buf.offset_of(it->data());
    bool success = ParsePacket(whole_buf.slice(field_offset, it->size()));
    if (PERFETTO_UNLIKELY(!success))
      return false;
  }

  const size_t bytes_left = decoder.bytes_left();
  if (bytes_left > 0) {
    PERFETTO_DCHECK(partial_buf_.empty());
    partial_buf_.insert(partial_buf_.end(), &data[decoder.read_offset()],
                        &data[decoder.read_offset() + bytes_left]);
  }
  return true;
}

bool ProtoTraceTokenizer::ParsePacket(TraceBlobView packet) {
  protos::pbzero::TracePacket::Decoder decoder(packet.data(), packet.length());
  if (PERFETTO_UNLIKELY(decoder.bytes_left()))
    return false;

  auto timestamp = decoder.has_timestamp()
                       ? static_cast<int64_t>(decoder.timestamp())
                       : latest_timestamp_;
  latest_timestamp_ = std::max(timestamp, latest_timestamp_);

  if (decoder.incremental_state_cleared()) {
    HandleIncrementalStateCleared(decoder);
  } else if (decoder.previous_packet_dropped()) {
    HandlePreviousPacketDropped(decoder);
  }

  if (decoder.has_interned_data()) {
    auto field = decoder.interned_data();
    const size_t offset = packet.offset_of(field.data);
    ParseInternedData(decoder, packet.slice(offset, field.size));
  }

  if (decoder.has_ftrace_events()) {
    auto ftrace_field = decoder.ftrace_events();
    const size_t fld_off = packet.offset_of(ftrace_field.data);
    ParseFtraceBundle(packet.slice(fld_off, ftrace_field.size));
    return true;
  }

  if (decoder.has_track_event()) {
    ParseTrackEventPacket(decoder, std::move(packet));
    return true;
  }

  if (decoder.has_thread_descriptor()) {
    ParseThreadDescriptorPacket(decoder);
    return true;
  }

  // Use parent data and length because we want to parse this again
  // later to get the exact type of the packet.
  context_->sorter->PushTracePacket(timestamp, std::move(packet));

  return true;
}

void ProtoTraceTokenizer::HandleIncrementalStateCleared(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG(
        "incremental_state_cleared without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }
  GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id())
      ->OnIncrementalStateCleared();
}

void ProtoTraceTokenizer::HandlePreviousPacketDropped(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("previous_packet_dropped without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }
  GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id())
      ->OnPacketLoss();
}

void ProtoTraceTokenizer::ParseInternedData(
    const protos::pbzero::TracePacket::Decoder& packet_decoder,
    TraceBlobView interned_data) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("InternedData packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());

  protos::pbzero::InternedData::Decoder interned_data_decoder(
      interned_data.data(), interned_data.length());

  // Store references to interned data submessages into the sequence's state.
  for (auto it = interned_data_decoder.event_categories(); it; ++it) {
    size_t offset = interned_data.offset_of(it->data());
    InternMessage<protos::pbzero::EventCategory>(
        context_, state, interned_data.slice(offset, it->size()));
  }

  for (auto it = interned_data_decoder.legacy_event_names(); it; ++it) {
    size_t offset = interned_data.offset_of(it->data());
    InternMessage<protos::pbzero::LegacyEventName>(
        context_, state, interned_data.slice(offset, it->size()));
  }

  for (auto it = interned_data_decoder.debug_annotation_names(); it; ++it) {
    size_t offset = interned_data.offset_of(it->data());
    InternMessage<protos::pbzero::DebugAnnotationName>(
        context_, state, interned_data.slice(offset, it->size()));
  }

  for (auto it = interned_data_decoder.source_locations(); it; ++it) {
    size_t offset = interned_data.offset_of(it->data());
    InternMessage<protos::pbzero::SourceLocation>(
        context_, state, interned_data.slice(offset, it->size()));
  }
}

void ProtoTraceTokenizer::ParseThreadDescriptorPacket(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("ThreadDescriptor packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());

  // TrackEvents will be ignored while incremental state is invalid. As a
  // consequence, we should also ignore any ThreadDescriptors received in this
  // state. Otherwise, any delta-encoded timestamps would be calculated
  // incorrectly once we move out of the packet loss state. Instead, wait until
  // the first subsequent descriptor after incremental state is cleared.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(
        stats::track_event_tokenizer_skipped_packets);
    return;
  }

  auto thread_descriptor_field = packet_decoder.thread_descriptor();
  protos::pbzero::ThreadDescriptor::Decoder thread_descriptor_decoder(
      thread_descriptor_field.data, thread_descriptor_field.size);

  state->SetThreadDescriptor(
      thread_descriptor_decoder.pid(), thread_descriptor_decoder.tid(),
      thread_descriptor_decoder.reference_timestamp_us() * 1000,
      thread_descriptor_decoder.reference_thread_time_us() * 1000);
  // TODO(eseckler): Handle other thread_descriptor fields (e.g. thread
  // name/type).
}

void ProtoTraceTokenizer::ParseTrackEventPacket(
    const protos::pbzero::TracePacket::Decoder& packet_decoder,
    TraceBlobView packet) {
  constexpr auto kTimestampDeltaUsFieldNumber =
      protos::pbzero::TrackEvent::kTimestampDeltaUsFieldNumber;
  constexpr auto kTimestampAbsoluteUsFieldNumber =
      protos::pbzero::TrackEvent::kTimestampAbsoluteUsFieldNumber;
  constexpr auto kThreadTimeDeltaUsFieldNumber =
      protos::pbzero::TrackEvent::kThreadTimeDeltaUsFieldNumber;
  constexpr auto kThreadTimeAbsoluteUsFieldNumber =
      protos::pbzero::TrackEvent::kThreadTimeAbsoluteUsFieldNumber;

  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("TrackEvent packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());

  // TrackEvents can only be parsed correctly while incremental state for their
  // sequence is valid and after a ThreadDescriptor has been parsed.
  if (!state->IsTrackEventStateValid()) {
    context_->storage->IncrementStats(
        stats::track_event_tokenizer_skipped_packets);
    return;
  }

  auto field = packet_decoder.track_event();
  ProtoDecoder event_decoder(field.data, field.size);

  int64_t timestamp;
  int64_t thread_timestamp = 0;

  if (auto ts_delta_field =
          event_decoder.FindField(kTimestampDeltaUsFieldNumber)) {
    timestamp = state->IncrementAndGetTrackEventTimeNs(
        ts_delta_field.as_int64() * 1000);
  } else if (auto ts_absolute_field =
                 event_decoder.FindField(kTimestampAbsoluteUsFieldNumber)) {
    // One-off absolute timestamps don't affect delta computation.
    timestamp = ts_absolute_field.as_int64() * 1000;
  } else {
    PERFETTO_ELOG("TrackEvent without timestamp");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  if (auto tt_delta_field =
          event_decoder.FindField(kThreadTimeDeltaUsFieldNumber)) {
    thread_timestamp = state->IncrementAndGetTrackEventThreadTimeNs(
        tt_delta_field.as_int64() * 1000);
  } else if (auto tt_absolute_field =
                 event_decoder.FindField(kThreadTimeAbsoluteUsFieldNumber)) {
    // One-off absolute timestamps don't affect delta computation.
    thread_timestamp = tt_absolute_field.as_int64() * 1000;
  }

  context_->sorter->PushTrackEventPacket(timestamp, thread_timestamp, state,
                                         std::move(packet));
}

PERFETTO_ALWAYS_INLINE
void ProtoTraceTokenizer::ParseFtraceBundle(TraceBlobView bundle) {
  protos::pbzero::FtraceEventBundle::Decoder decoder(bundle.data(),
                                                     bundle.length());

  if (PERFETTO_UNLIKELY(!decoder.has_cpu())) {
    PERFETTO_ELOG("CPU field not found in FtraceEventBundle");
    context_->storage->IncrementStats(stats::ftrace_bundle_tokenizer_errors);
    return;
  }

  uint32_t cpu = decoder.cpu();
  if (PERFETTO_UNLIKELY(cpu > base::kMaxCpus)) {
    PERFETTO_ELOG("CPU larger than kMaxCpus (%u > %zu)", cpu, base::kMaxCpus);
    return;
  }

  for (auto it = decoder.event(); it; ++it) {
    size_t off = bundle.offset_of(it->data());
    ParseFtraceEvent(cpu, bundle.slice(off, it->size()));
  }
  context_->sorter->FinalizeFtraceEventBatch(cpu);
}

PERFETTO_ALWAYS_INLINE
void ProtoTraceTokenizer::ParseFtraceEvent(uint32_t cpu, TraceBlobView event) {
  constexpr auto kTimestampFieldNumber =
      protos::pbzero::FtraceEvent::kTimestampFieldNumber;
  const uint8_t* data = event.data();
  const size_t length = event.length();
  ProtoDecoder decoder(data, length);
  uint64_t raw_timestamp = 0;
  bool timestamp_found = false;

  // Speculate on the fact that the timestamp is often the 1st field of the
  // event.
  constexpr auto timestampFieldTag = MakeTagVarInt(kTimestampFieldNumber);
  if (PERFETTO_LIKELY(length > 10 && data[0] == timestampFieldTag)) {
    // Fastpath.
    const uint8_t* next = ParseVarInt(data + 1, data + 11, &raw_timestamp);
    timestamp_found = next != data + 1;
    decoder.Reset(next);
  } else {
    // Slowpath.
    if (auto ts_field = decoder.FindField(kTimestampFieldNumber)) {
      timestamp_found = true;
      raw_timestamp = ts_field.as_uint64();
    }
  }

  if (PERFETTO_UNLIKELY(!timestamp_found)) {
    PERFETTO_ELOG("Timestamp field not found in FtraceEvent");
    context_->storage->IncrementStats(stats::ftrace_bundle_tokenizer_errors);
    return;
  }

  int64_t timestamp = static_cast<int64_t>(raw_timestamp);
  latest_timestamp_ = std::max(timestamp, latest_timestamp_);

  // We don't need to parse this packet, just push it to be sorted with
  // the timestamp.
  context_->sorter->PushFtraceEvent(cpu, timestamp, std::move(event));
}

}  // namespace trace_processor
}  // namespace perfetto
