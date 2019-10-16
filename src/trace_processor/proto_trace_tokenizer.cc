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

#include <zlib.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_incremental_state.h"
#include "src/trace_processor/stats.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/trace_storage.h"
#include "src/trace_processor/track_tracker.h"

#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "protos/perfetto/trace/track_event/task_execution.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

using protozero::ProtoDecoder;
using protozero::proto_utils::MakeTagLengthDelimited;
using protozero::proto_utils::MakeTagVarInt;
using protozero::proto_utils::ParseVarInt;

namespace {

constexpr uint8_t kTracePacketTag =
    MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);

TraceBlobView Decompress(TraceBlobView input) {
  uint8_t out[4096];
  std::string s;

  z_stream stream{};
  stream.next_in = const_cast<uint8_t*>(input.data());
  stream.avail_in = static_cast<unsigned int>(input.length());

  if (inflateInit(&stream) != Z_OK)
    return TraceBlobView(nullptr, 0, 0);

  int ret;
  do {
    stream.next_out = out;
    stream.avail_out = sizeof(out);
    ret = inflate(&stream, Z_NO_FLUSH);
    if (ret != Z_STREAM_END && ret != Z_OK)
      return TraceBlobView(nullptr, 0, 0);
    s.append(reinterpret_cast<char*>(out), sizeof(out) - stream.avail_out);
  } while (ret != Z_STREAM_END);
  inflateEnd(&stream);

  std::unique_ptr<uint8_t[]> output(new uint8_t[s.size()]);
  memcpy(output.get(), s.data(), s.size());
  return TraceBlobView(std::move(output), 0, s.size());
}

}  // namespace

ProtoTraceTokenizer::ProtoTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}
ProtoTraceTokenizer::~ProtoTraceTokenizer() = default;

util::Status ProtoTraceTokenizer::Parse(std::unique_ptr<uint8_t[]> owned_buf,
                                        size_t size) {
  uint8_t* data = &owned_buf[0];
  if (!partial_buf_.empty()) {
    // It takes ~5 bytes for a proto preamble + the varint size.
    const size_t kHeaderBytes = 5;
    if (PERFETTO_UNLIKELY(partial_buf_.size() < kHeaderBytes)) {
      size_t missing_len = std::min(kHeaderBytes - partial_buf_.size(), size);
      partial_buf_.insert(partial_buf_.end(), &data[0], &data[missing_len]);
      if (partial_buf_.size() < kHeaderBytes)
        return util::OkStatus();
      data += missing_len;
      size -= missing_len;
    }

    // At this point we have enough data in |partial_buf_| to read at least the
    // field header and know the size of the next TracePacket.
    const uint8_t* pos = &partial_buf_[0];
    uint8_t proto_field_tag = *pos;
    uint64_t field_size = 0;
    const uint8_t* next = ParseVarInt(++pos, &*partial_buf_.end(), &field_size);
    bool parse_failed = next == pos;
    pos = next;
    if (proto_field_tag != kTracePacketTag || field_size == 0 || parse_failed) {
      return util::ErrStatus(
          "Failed parsing a TracePacket from the partial buffer");
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
      util::Status status =
          ParseInternal(std::move(buf), buf_start, size_incl_header);
      if (PERFETTO_UNLIKELY(!status.ok()))
        return status;
    } else {
      partial_buf_.insert(partial_buf_.end(), data, &data[size]);
      return util::OkStatus();
    }
  }
  return ParseInternal(std::move(owned_buf), data, size);
}

util::Status ProtoTraceTokenizer::ParseInternal(
    std::unique_ptr<uint8_t[]> owned_buf,
    uint8_t* data,
    size_t size) {
  PERFETTO_DCHECK(data >= &owned_buf[0]);
  const uint8_t* start = &owned_buf[0];
  const size_t data_off = static_cast<size_t>(data - start);
  TraceBlobView whole_buf(std::move(owned_buf), data_off, size);

  protos::pbzero::Trace::Decoder decoder(data, size);
  for (auto it = decoder.packet(); it; ++it) {
    size_t field_offset = whole_buf.offset_of(it->data());
    util::Status status =
        ParsePacket(whole_buf.slice(field_offset, it->size()));
    if (PERFETTO_UNLIKELY(!status.ok()))
      return status;
  }

  const size_t bytes_left = decoder.bytes_left();
  if (bytes_left > 0) {
    PERFETTO_DCHECK(partial_buf_.empty());
    partial_buf_.insert(partial_buf_.end(), &data[decoder.read_offset()],
                        &data[decoder.read_offset() + bytes_left]);
  }
  return util::OkStatus();
}

util::Status ProtoTraceTokenizer::ParsePacket(TraceBlobView packet) {
  protos::pbzero::TracePacket::Decoder decoder(packet.data(), packet.length());
  if (PERFETTO_UNLIKELY(decoder.bytes_left()))
    return util::ErrStatus(
        "Failed to parse proto packet fully; the trace is probably corrupt.");

  auto timestamp = decoder.has_timestamp()
                       ? static_cast<int64_t>(decoder.timestamp())
                       : latest_timestamp_;

  const uint32_t seq_id = decoder.trusted_packet_sequence_id();

  // If the TracePacket specifies a non-zero clock-id, translate the timestamp
  // into the trace-time clock domain.
  if (decoder.timestamp_clock_id()) {
    PERFETTO_DCHECK(decoder.has_timestamp());
    ClockTracker::ClockId clock_id = decoder.timestamp_clock_id();
    bool is_seq_scoped = ClockTracker::IsReservedSeqScopedClockId(clock_id);
    if (is_seq_scoped) {
      if (!seq_id) {
        return util::ErrStatus(
            "TracePacket specified a sequence-local clock id (%" PRIu32
            ") but the TraceWriter's sequence_id is zero (the service is "
            "probably too old)",
            decoder.timestamp_clock_id());
      }
      clock_id = ClockTracker::SeqScopedClockIdToGlobal(
          seq_id, decoder.timestamp_clock_id());
    }
    auto trace_ts = context_->clock_tracker->ToTraceTime(clock_id, timestamp);
    if (!trace_ts.has_value()) {
      // ToTraceTime() will increase the |clock_sync_failure| stat on failure.
      static const char seq_extra_err[] =
          " Because the clock id is sequence-scoped, the ClockSnapshot must be "
          "emitted on the same TraceWriter sequence of the packet that refers "
          "to that clock id.";
      return util::ErrStatus(
          "Failed to convert TracePacket's timestamp from clock_id=%" PRIu32
          " seq_id=%" PRIu32
          ". This is usually due to the lack of a prior ClockSnapshot proto.%s",
          decoder.timestamp_clock_id(), seq_id,
          is_seq_scoped ? seq_extra_err : "");
    }
    timestamp = trace_ts.value();
  } else if (decoder.has_chrome_events() || decoder.has_chrome_metadata()) {
    // Chrome timestamps are in MONOTONIC domain. Adjust to trace time if we
    // have a clock snapshot.
    // TODO(eseckler): Set timestamp_clock_id in chrome and then remove this.
    auto trace_ts = context_->clock_tracker->ToTraceTime(
        protos::pbzero::ClockSnapshot::Clock::MONOTONIC, timestamp);
    if (trace_ts.has_value())
      timestamp = trace_ts.value();
  }
  latest_timestamp_ = std::max(timestamp, latest_timestamp_);

  auto* state = GetIncrementalStateForPacketSequence(
      decoder.trusted_packet_sequence_id());

  uint32_t sequence_flags = decoder.sequence_flags();

  if (decoder.incremental_state_cleared() ||
      sequence_flags &
          protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED) {
    HandleIncrementalStateCleared(decoder);
  } else if (decoder.previous_packet_dropped()) {
    HandlePreviousPacketDropped(decoder);
  }

  if (decoder.sequence_flags() &
      protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE) {
    if (!seq_id) {
      return util::ErrStatus(
          "TracePacket specified SEQ_NEEDS_INCREMENTAL_STATE but the "
          "TraceWriter's sequence_id is zero (the service is "
          "probably too old)");
    }

    if (!state->IsIncrementalStateValid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return util::OkStatus();
    }
  }

  if (decoder.has_clock_snapshot()) {
    return ParseClockSnapshot(decoder.clock_snapshot(),
                              decoder.trusted_packet_sequence_id());
  }

  // TODO(eseckler): Parse TracePacketDefaults.

  if (decoder.has_interned_data()) {
    auto field = decoder.interned_data();
    const size_t offset = packet.offset_of(field.data);
    ParseInternedData(decoder, packet.slice(offset, field.size));
  }

  if (decoder.has_ftrace_events()) {
    auto ftrace_field = decoder.ftrace_events();
    const size_t fld_off = packet.offset_of(ftrace_field.data);
    ParseFtraceBundle(packet.slice(fld_off, ftrace_field.size));
    return util::OkStatus();
  }

  if (decoder.has_track_descriptor()) {
    ParseTrackDescriptorPacket(decoder);
    return util::OkStatus();
  }

  if (decoder.has_track_event()) {
    ParseTrackEventPacket(decoder, std::move(packet), timestamp);
    return util::OkStatus();
  }

  // TODO(eseckler): Remove this once Chrome has switched fully over to
  // TrackDescriptors.
  if (decoder.has_thread_descriptor()) {
    ParseThreadDescriptorPacket(decoder);
    return util::OkStatus();
  }
  if (decoder.has_process_descriptor()) {
    ParseProcessDescriptorPacket(decoder);
    return util::OkStatus();
  }

  if (decoder.has_compressed_packets()) {
    protozero::ConstBytes field = decoder.compressed_packets();
    const size_t field_off = packet.offset_of(field.data);
    TraceBlobView compressed_packets = packet.slice(field_off, field.size);
    TraceBlobView packets = Decompress(std::move(compressed_packets));

    const uint8_t* start = packets.data();
    const uint8_t* end = packets.data() + packets.length();
    const uint8_t* ptr = start;
    while ((end - ptr) > 2) {
      const uint8_t* packet_start = ptr;
      if (PERFETTO_UNLIKELY(*ptr != kTracePacketTag))
        return util::ErrStatus("Expected TracePacket tag");
      uint64_t packet_size = 0;
      ptr = ParseVarInt(++ptr, end, &packet_size);
      size_t packet_offset = static_cast<size_t>(ptr - start);
      ptr += packet_size;
      if (PERFETTO_UNLIKELY((ptr - packet_start) < 2 || ptr > end))
        return util::ErrStatus("Invalid packet size");
      util::Status status = ParsePacket(
          packets.slice(packet_offset, static_cast<size_t>(packet_size)));
      if (PERFETTO_UNLIKELY(!status.ok()))
        return status;
    }

    return util::OkStatus();
  }

  if (decoder.has_trace_config()) {
    auto config = decoder.trace_config();
    protos::pbzero::TraceConfig::Decoder trace_config(config.data, config.size);

    if (trace_config.write_into_file()) {
      int64_t window_size_ns;
      if (trace_config.has_flush_period_ms() &&
          trace_config.flush_period_ms() > 0) {
        // We use 2x the flush period as a margin of error to allow for any
        // late flush responses to still be sorted correctly.
        window_size_ns = static_cast<int64_t>(trace_config.flush_period_ms()) *
                         2 * 1000 * 1000;
      } else {
        constexpr uint64_t kDefaultWindowNs =
            180 * 1000 * 1000 * 1000ULL;  // 3 minutes.
        PERFETTO_ELOG(
            "It is strongly recommended to have flush_period_ms set when "
            "write_into_file is turned on. You will likely have many dropped "
            "events because of inability to sort the events correctly.");
        window_size_ns = static_cast<int64_t>(kDefaultWindowNs);
      }
      context_->sorter->SetWindowSizeNs(window_size_ns);
    }
  }

  // Use parent data and length because we want to parse this again
  // later to get the exact type of the packet.
  context_->sorter->PushTracePacket(timestamp, state, std::move(packet));

  return util::OkStatus();
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

  // Don't parse interned data entries until incremental state is valid, because
  // they could otherwise be associated with the wrong generation in the state.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
    return;
  }

  // Store references to interned data submessages into the sequence's state.
  protozero::ProtoDecoder decoder(interned_data.data(), interned_data.length());
  for (protozero::Field f = decoder.ReadField(); f.valid();
       f = decoder.ReadField()) {
    auto bytes = f.as_bytes();
    auto offset = interned_data.offset_of(bytes.data);
    state->InternMessage(f.id(), interned_data.slice(offset, bytes.size));
  }
}

void ProtoTraceTokenizer::ParseTrackDescriptorPacket(
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

    ParseThreadDescriptor(thread_descriptor_decoder);
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

void ProtoTraceTokenizer::ParseProcessDescriptorPacket(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  protos::pbzero::ProcessDescriptor::Decoder process_descriptor_decoder(
      packet_decoder.process_descriptor());
  if (!process_descriptor_decoder.has_chrome_process_type())
    return;
  base::StringView name = "Unknown";
  switch (process_descriptor_decoder.chrome_process_type()) {
    case protos::pbzero::ProcessDescriptor::PROCESS_BROWSER:
      name = "Browser";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_RENDERER:
      name = "Renderer";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_UTILITY:
      name = "Utility";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_ZYGOTE:
      name = "Zygote";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_SANDBOX_HELPER:
      name = "SandboxHelper";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_GPU:
      name = "Gpu";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_PPAPI_PLUGIN:
      name = "PpapiPlugin";
      break;
    case protos::pbzero::ProcessDescriptor::PROCESS_PPAPI_BROKER:
      name = "PpapiBroker";
      break;
  }
  context_->process_tracker->SetProcessMetadata(
      static_cast<uint32_t>(process_descriptor_decoder.pid()), base::nullopt,
      name);
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

  ParseThreadDescriptor(thread_descriptor_decoder);
}

void ProtoTraceTokenizer::ParseThreadDescriptor(
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
    procs->UpdateThreadName(
        static_cast<uint32_t>(thread_descriptor_decoder.tid()), thread_name_id);
  }
}

util::Status ProtoTraceTokenizer::ParseClockSnapshot(ConstBytes blob,
                                                     uint32_t seq_id) {
  std::map<ClockTracker::ClockId, int64_t> clock_map;
  protos::pbzero::ClockSnapshot::Decoder evt(blob.data, blob.size);
  for (auto it = evt.clocks(); it; ++it) {
    protos::pbzero::ClockSnapshot::Clock::Decoder clk(it->data(), it->size());
    ClockTracker::ClockId clock_id = clk.clock_id();
    if (ClockTracker::IsReservedSeqScopedClockId(clk.clock_id())) {
      if (!seq_id) {
        return util::ErrStatus(
            "ClockSnapshot packet is specifying a sequence-scoped clock id "
            "(%" PRIu64 ") but the TracePacket sequence_id is zero",
            clock_id);
      }
      clock_id = ClockTracker::SeqScopedClockIdToGlobal(seq_id, clk.clock_id());
    }
    clock_map[clock_id] = static_cast<int64_t>(clk.timestamp());
  }
  context_->clock_tracker->AddSnapshot(clock_map);
  return util::OkStatus();
}

void ProtoTraceTokenizer::ParseTrackEventPacket(
    const protos::pbzero::TracePacket::Decoder& packet_decoder,
    TraceBlobView packet,
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

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());

  // TODO(eseckler): For now, TrackEvents can only be parsed correctly while
  // incremental state for their sequence is valid, because chromium doesn't set
  // SEQ_NEEDS_INCREMENTAL_STATE yet. Remove this once it does.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
    return;
  }

  auto field = packet_decoder.track_event();
  ProtoDecoder event_decoder(field.data, field.size);

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
  } else if (auto ts_absolute_field =
                 event_decoder.FindField(kTimestampAbsoluteUsFieldNumber)) {
    // One-off absolute timestamps don't affect delta computation.
    timestamp = ts_absolute_field.as_int64() * 1000;

    // Legacy TrackEvent timestamp fields are in MONOTONIC domain. Adjust to
    // trace time if we have a clock snapshot.
    auto trace_ts = context_->clock_tracker->ToTraceTime(
        protos::pbzero::ClockSnapshot::Clock::MONOTONIC, timestamp);
    if (trace_ts.has_value())
      timestamp = trace_ts.value();
  } else if (packet_decoder.has_timestamp()) {
    timestamp = packet_timestamp;
  } else {
    PERFETTO_ELOG("TrackEvent without timestamp");
    context_->storage->IncrementStats(stats::track_event_tokenizer_errors);
    return;
  }

  latest_timestamp_ = std::max(timestamp, latest_timestamp_);

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

  if (decoder.has_compact_sched()) {
    ParseFtraceCompactSched(cpu, decoder.compact_sched().data,
                            decoder.compact_sched().size);
  }

  for (auto it = decoder.event(); it; ++it) {
    size_t off = bundle.offset_of(it->data());
    ParseFtraceEvent(cpu, bundle.slice(off, it->size()));
  }
  context_->sorter->FinalizeFtraceEventBatch(cpu);
}

void ProtoTraceTokenizer::ParseFtraceCompactSched(uint32_t cpu,
                                                  const uint8_t* data,
                                                  size_t size) {
  protos::pbzero::FtraceEventBundle_CompactSched::Decoder compact(data, size);

  // Build the interning table for next_comm fields.
  std::vector<StringId> string_table;
  string_table.reserve(512);
  for (auto it = compact.switch_next_comm_table(); it; it++) {
    StringId value = context_->storage->InternString(it->as_string());
    string_table.push_back(value);
  }

  // Accumulator for timestamp deltas.
  int64_t timestamp_acc = 0;

  // The events' fields are stored in a structure-of-arrays style, using packed
  // repeated fields. Walk each repeated field in step to recover individual
  // events.
  bool parse_error = false;
  auto timestamp_it = compact.switch_timestamp(&parse_error);
  auto pstate_it = compact.switch_prev_state(&parse_error);
  auto npid_it = compact.switch_next_pid(&parse_error);
  auto nprio_it = compact.switch_next_prio(&parse_error);
  auto comm_it = compact.switch_next_comm_index(&parse_error);
  for (; timestamp_it && pstate_it && npid_it && nprio_it && comm_it;
       ++timestamp_it, ++pstate_it, ++npid_it, ++nprio_it, ++comm_it) {
    InlineSchedSwitch event{};

    // delta-encoded timestamp
    timestamp_acc += static_cast<int64_t>(*timestamp_it);
    int64_t event_timestamp = timestamp_acc;

    // index into the interned string table
    PERFETTO_DCHECK(*comm_it < string_table.size());
    event.next_comm = string_table[*comm_it];

    event.prev_state = *pstate_it;
    event.next_pid = *npid_it;
    event.next_prio = *nprio_it;

    context_->sorter->PushInlineFtraceEvent(cpu, event_timestamp,
                                            InlineEvent::SchedSwitch(event));
  }

  // Check that all packed buffers were decoded correctly, and fully.
  bool sizes_match =
      !timestamp_it && !pstate_it && !npid_it && !nprio_it && !comm_it;
  if (parse_error || !sizes_match)
    context_->storage->IncrementStats(stats::compact_sched_has_parse_errors);

  latest_timestamp_ = std::max(timestamp_acc, latest_timestamp_);
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
