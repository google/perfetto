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

#include "src/trace_processor/importers/proto/proto_trace_reader.h"

#include <optional>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_module.h"
#include "src/trace_processor/importers/proto/packet_analyzer.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/gzip_utils.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/common/trace_stats.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/extension_descriptor.pbzero.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

ProtoTraceReader::ProtoTraceReader(TraceProcessorContext* ctx)
    : context_(ctx),
      skipped_packet_key_id_(ctx->storage->InternString("skipped_packet")),
      invalid_incremental_state_key_id_(
          ctx->storage->InternString("invalid_incremental_state")) {}
ProtoTraceReader::~ProtoTraceReader() = default;

util::Status ProtoTraceReader::Parse(TraceBlobView blob) {
  return tokenizer_.Tokenize(std::move(blob), [this](TraceBlobView packet) {
    return ParsePacket(std::move(packet));
  });
}

util::Status ProtoTraceReader::ParseExtensionDescriptor(ConstBytes descriptor) {
  protos::pbzero::ExtensionDescriptor::Decoder decoder(descriptor.data,
                                                       descriptor.size);

  auto extension = decoder.extension_set();
  return context_->descriptor_pool_->AddFromFileDescriptorSet(
      extension.data, extension.size,
      /*skip_prefixes*/ {},
      /*merge_existing_messages=*/true);
}

util::Status ProtoTraceReader::ParsePacket(TraceBlobView packet) {
  protos::pbzero::TracePacket::Decoder decoder(packet.data(), packet.length());
  if (PERFETTO_UNLIKELY(decoder.bytes_left())) {
    return util::ErrStatus(
        "Failed to parse proto packet fully; the trace is probably corrupt.");
  }

  // Any compressed packets should have been handled by the tokenizer.
  PERFETTO_CHECK(!decoder.has_compressed_packets());

  // When the trace packet is emitted from a remote machine: parse the packet
  // using a different ProtoTraceReader instance. The packet will be parsed
  // in the context of the remote machine.
  if (PERFETTO_UNLIKELY(decoder.has_machine_id())) {
    if (!context_->machine_id()) {
      // Default context: switch to another reader instance to parse the packet.
      PERFETTO_DCHECK(context_->multi_machine_trace_manager);
      auto* reader = context_->multi_machine_trace_manager->GetOrCreateReader(
          decoder.machine_id());
      return reader->ParsePacket(std::move(packet));
    }
  }
  // Assert that the packet is parsed using the right instance of reader.
  PERFETTO_DCHECK(decoder.has_machine_id() == !!context_->machine_id());

  const uint32_t seq_id = decoder.trusted_packet_sequence_id();
  auto* state = GetIncrementalStateForPacketSequence(seq_id);

  if (decoder.first_packet_on_sequence()) {
    HandleFirstPacketOnSequence(seq_id);
  }

  uint32_t sequence_flags = decoder.sequence_flags();

  if (decoder.incremental_state_cleared() ||
      sequence_flags &
          protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED) {
    HandleIncrementalStateCleared(decoder);
  } else if (decoder.previous_packet_dropped()) {
    HandlePreviousPacketDropped(decoder);
  }

  uint32_t sequence_id = decoder.trusted_packet_sequence_id();
  if (sequence_id) {
    auto [data_loss, inserted] =
        packet_sequence_data_loss_.Insert(sequence_id, 0);

    if (!inserted && decoder.previous_packet_dropped()) {
      *data_loss += 1;
    }
  }

  // It is important that we parse defaults before parsing other fields such as
  // the timestamp, since the defaults could affect them.
  if (decoder.has_trace_packet_defaults()) {
    auto field = decoder.trace_packet_defaults();
    ParseTracePacketDefaults(decoder, packet.slice(field.data, field.size));
  }

  if (decoder.has_interned_data()) {
    auto field = decoder.interned_data();
    ParseInternedData(decoder, packet.slice(field.data, field.size));
  }

  if (decoder.has_clock_snapshot()) {
    return ParseClockSnapshot(decoder.clock_snapshot(), sequence_id);
  }

  if (decoder.has_trace_stats()) {
    ParseTraceStats(decoder.trace_stats());
  }

  if (decoder.has_service_event()) {
    PERFETTO_DCHECK(decoder.has_timestamp());
    int64_t ts = static_cast<int64_t>(decoder.timestamp());
    return ParseServiceEvent(ts, decoder.service_event());
  }

  if (decoder.has_extension_descriptor()) {
    return ParseExtensionDescriptor(decoder.extension_descriptor());
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
      if (context_->content_analyzer) {
        // Account for the skipped packet for trace proto content analysis,
        // with a special annotation.
        PacketAnalyzer::SampleAnnotation annotation;
        annotation.push_back(
            {skipped_packet_key_id_, invalid_incremental_state_key_id_});
        PacketAnalyzer::Get(context_)->ProcessPacket(packet, annotation);
      }
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return util::OkStatus();
    }
  }

  protos::pbzero::TracePacketDefaults::Decoder* defaults =
      state->current_generation()->GetTracePacketDefaults();

  int64_t timestamp;
  if (decoder.has_timestamp()) {
    timestamp = static_cast<int64_t>(decoder.timestamp());

    uint32_t timestamp_clock_id =
        decoder.has_timestamp_clock_id()
            ? decoder.timestamp_clock_id()
            : (defaults ? defaults->timestamp_clock_id() : 0);

    if ((decoder.has_chrome_events() || decoder.has_chrome_metadata()) &&
        (!timestamp_clock_id ||
         timestamp_clock_id == protos::pbzero::BUILTIN_CLOCK_MONOTONIC)) {
      // Chrome event timestamps are in MONOTONIC domain, but may occur in
      // traces where (a) no clock snapshots exist or (b) no clock_id is
      // specified for their timestamps. Adjust to trace time if we have a clock
      // snapshot.
      // TODO(eseckler): Set timestamp_clock_id and emit ClockSnapshots in
      // chrome and then remove this.
      auto trace_ts = context_->clock_tracker->ToTraceTime(
          protos::pbzero::BUILTIN_CLOCK_MONOTONIC, timestamp);
      if (trace_ts.ok())
        timestamp = trace_ts.value();
    } else if (timestamp_clock_id) {
      // If the TracePacket specifies a non-zero clock-id, translate the
      // timestamp into the trace-time clock domain.
      ClockTracker::ClockId converted_clock_id = timestamp_clock_id;
      if (ClockTracker::IsSequenceClock(converted_clock_id)) {
        if (!seq_id) {
          return util::ErrStatus(
              "TracePacket specified a sequence-local clock id (%" PRIu32
              ") but the TraceWriter's sequence_id is zero (the service is "
              "probably too old)",
              timestamp_clock_id);
        }
        converted_clock_id =
            ClockTracker::SequenceToGlobalClock(seq_id, timestamp_clock_id);
      }
      auto trace_ts =
          context_->clock_tracker->ToTraceTime(converted_clock_id, timestamp);
      if (!trace_ts.ok()) {
        // ToTraceTime() will increase the |clock_sync_failure| stat on failure.
        // We don't return an error here as it will cause the trace to stop
        // parsing. Instead, we rely on the stat increment in ToTraceTime() to
        // inform the user about the error.
        return util::OkStatus();
      }
      timestamp = trace_ts.value();
    }
  } else {
    timestamp = std::max(latest_timestamp_, context_->sorter->max_timestamp());
  }
  latest_timestamp_ = std::max(timestamp, latest_timestamp_);

  if (context_->content_analyzer && !decoder.has_track_event()) {
    PacketAnalyzer::Get(context_)->ProcessPacket(packet, {});
  }

  auto& modules = context_->modules_by_field;
  for (uint32_t field_id = 1; field_id < modules.size(); ++field_id) {
    if (!modules[field_id].empty() && decoder.Get(field_id).valid()) {
      for (ProtoImporterModule* global_module :
           context_->modules_for_all_fields) {
        ModuleResult res = global_module->TokenizePacket(
            decoder, &packet, timestamp, state->current_generation(), field_id);
        if (!res.ignored())
          return res.ToStatus();
      }
      for (ProtoImporterModule* module : modules[field_id]) {
        ModuleResult res = module->TokenizePacket(
            decoder, &packet, timestamp, state->current_generation(), field_id);
        if (!res.ignored())
          return res.ToStatus();
      }
    }
  }

  if (decoder.has_trace_config()) {
    ParseTraceConfig(decoder.trace_config());
  }

  // Use parent data and length because we want to parse this again
  // later to get the exact type of the packet.
  context_->sorter->PushTracePacket(timestamp, state->current_generation(),
                                    std::move(packet), context_->machine_id());

  return util::OkStatus();
}

void ProtoTraceReader::ParseTraceConfig(protozero::ConstBytes blob) {
  protos::pbzero::TraceConfig::Decoder trace_config(blob);
  if (trace_config.write_into_file() && !trace_config.flush_period_ms()) {
    PERFETTO_ELOG(
        "It is strongly recommended to have flush_period_ms set when "
        "write_into_file is turned on. This trace will be loaded fully "
        "into memory before sorting which increases the likelihood of "
        "OOMs.");
  }
}

void ProtoTraceReader::HandleIncrementalStateCleared(
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
  for (auto& module : context_->modules) {
    module->OnIncrementalStateCleared(
        packet_decoder.trusted_packet_sequence_id());
  }
}

void ProtoTraceReader::HandleFirstPacketOnSequence(
    uint32_t packet_sequence_id) {
  for (auto& module : context_->modules) {
    module->OnFirstPacketOnSequence(packet_sequence_id);
  }
}

void ProtoTraceReader::HandlePreviousPacketDropped(
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

void ProtoTraceReader::ParseTracePacketDefaults(
    const protos::pbzero::TracePacket_Decoder& packet_decoder,
    TraceBlobView trace_packet_defaults) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG(
        "TracePacketDefaults packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());
  state->UpdateTracePacketDefaults(std::move(trace_packet_defaults));
}

void ProtoTraceReader::ParseInternedData(
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
    state->InternMessage(f.id(), interned_data.slice(bytes.data, bytes.size));
  }
}

util::Status ProtoTraceReader::ParseClockSnapshot(ConstBytes blob,
                                                  uint32_t seq_id) {
  std::vector<ClockTracker::ClockTimestamp> clock_timestamps;
  protos::pbzero::ClockSnapshot::Decoder evt(blob.data, blob.size);
  if (evt.primary_trace_clock()) {
    context_->clock_tracker->SetTraceTimeClock(
        static_cast<ClockTracker::ClockId>(evt.primary_trace_clock()));
  }
  for (auto it = evt.clocks(); it; ++it) {
    protos::pbzero::ClockSnapshot::Clock::Decoder clk(*it);
    ClockTracker::ClockId clock_id = clk.clock_id();
    if (ClockTracker::IsSequenceClock(clk.clock_id())) {
      if (!seq_id) {
        return util::ErrStatus(
            "ClockSnapshot packet is specifying a sequence-scoped clock id "
            "(%" PRIu64 ") but the TracePacket sequence_id is zero",
            clock_id);
      }
      clock_id = ClockTracker::SequenceToGlobalClock(seq_id, clk.clock_id());
    }
    int64_t unit_multiplier_ns =
        clk.unit_multiplier_ns()
            ? static_cast<int64_t>(clk.unit_multiplier_ns())
            : 1;
    clock_timestamps.emplace_back(clock_id, clk.timestamp(), unit_multiplier_ns,
                                  clk.is_incremental());
  }

  base::StatusOr<uint32_t> snapshot_id =
      context_->clock_tracker->AddSnapshot(clock_timestamps);
  if (!snapshot_id.ok()) {
    PERFETTO_ELOG("%s", snapshot_id.status().c_message());
    return base::OkStatus();
  }

  std::optional<int64_t> trace_time_from_snapshot =
      context_->clock_tracker->ToTraceTimeFromSnapshot(clock_timestamps);

  // Add the all the clock snapshots to the clock snapshot table.
  std::optional<int64_t> trace_ts_for_check;
  for (const auto& clock_timestamp : clock_timestamps) {
    // If the clock is incremental, we need to use 0 to map correctly to
    // |absolute_timestamp|.
    int64_t ts_to_convert =
        clock_timestamp.clock.is_incremental ? 0 : clock_timestamp.timestamp;
    // Even if we have trace time from snapshot, we still run ToTraceTime to
    // optimise future conversions.
    base::StatusOr<int64_t> opt_trace_ts = context_->clock_tracker->ToTraceTime(
        clock_timestamp.clock.id, ts_to_convert);

    if (!opt_trace_ts.ok()) {
      // This can happen if |AddSnapshot| failed to resolve this clock, e.g. if
      // clock is not monotonic. Try to fetch trace time from snapshot.
      if (!trace_time_from_snapshot) {
        PERFETTO_DLOG("%s", opt_trace_ts.status().c_message());
        continue;
      }
      opt_trace_ts = *trace_time_from_snapshot;
    }

    // Double check that all the clocks in this snapshot resolve to the same
    // trace timestamp value.
    PERFETTO_DCHECK(!trace_ts_for_check ||
                    opt_trace_ts.value() == trace_ts_for_check.value());
    trace_ts_for_check = *opt_trace_ts;

    tables::ClockSnapshotTable::Row row;
    row.ts = *opt_trace_ts;
    row.clock_id = static_cast<int64_t>(clock_timestamp.clock.id);
    row.clock_value =
        clock_timestamp.timestamp * clock_timestamp.clock.unit_multiplier_ns;
    row.clock_name = GetBuiltinClockNameOrNull(clock_timestamp.clock.id);
    row.snapshot_id = *snapshot_id;
    row.machine_id = context_->machine_id();

    context_->storage->mutable_clock_snapshot_table()->Insert(row);
  }
  return util::OkStatus();
}

std::optional<StringId> ProtoTraceReader::GetBuiltinClockNameOrNull(
    int64_t clock_id) {
  switch (clock_id) {
    case protos::pbzero::ClockSnapshot::Clock::REALTIME:
      return context_->storage->InternString("REALTIME");
    case protos::pbzero::ClockSnapshot::Clock::REALTIME_COARSE:
      return context_->storage->InternString("REALTIME_COARSE");
    case protos::pbzero::ClockSnapshot::Clock::MONOTONIC:
      return context_->storage->InternString("MONOTONIC");
    case protos::pbzero::ClockSnapshot::Clock::MONOTONIC_COARSE:
      return context_->storage->InternString("MONOTONIC_COARSE");
    case protos::pbzero::ClockSnapshot::Clock::MONOTONIC_RAW:
      return context_->storage->InternString("MONOTONIC_RAW");
    case protos::pbzero::ClockSnapshot::Clock::BOOTTIME:
      return context_->storage->InternString("BOOTTIME");
    default:
      return std::nullopt;
  }
}

util::Status ProtoTraceReader::ParseServiceEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::TracingServiceEvent::Decoder tse(blob);
  if (tse.tracing_started()) {
    context_->metadata_tracker->SetMetadata(metadata::tracing_started_ns,
                                            Variadic::Integer(ts));
  }
  if (tse.tracing_disabled()) {
    context_->metadata_tracker->SetMetadata(metadata::tracing_disabled_ns,
                                            Variadic::Integer(ts));
  }
  if (tse.all_data_sources_started()) {
    context_->metadata_tracker->SetMetadata(
        metadata::all_data_source_started_ns, Variadic::Integer(ts));
  }
  if (tse.all_data_sources_flushed()) {
    context_->metadata_tracker->AppendMetadata(
        metadata::all_data_source_flushed_ns, Variadic::Integer(ts));
    context_->sorter->NotifyFlushEvent();
  }
  if (tse.read_tracing_buffers_completed()) {
    context_->sorter->NotifyReadBufferEvent();
  }
  return util::OkStatus();
}

void ProtoTraceReader::ParseTraceStats(ConstBytes blob) {
  protos::pbzero::TraceStats::Decoder evt(blob.data, blob.size);
  auto* storage = context_->storage.get();
  storage->SetStats(stats::traced_producers_connected,
                    static_cast<int64_t>(evt.producers_connected()));
  storage->SetStats(stats::traced_producers_seen,
                    static_cast<int64_t>(evt.producers_seen()));
  storage->SetStats(stats::traced_data_sources_registered,
                    static_cast<int64_t>(evt.data_sources_registered()));
  storage->SetStats(stats::traced_data_sources_seen,
                    static_cast<int64_t>(evt.data_sources_seen()));
  storage->SetStats(stats::traced_tracing_sessions,
                    static_cast<int64_t>(evt.tracing_sessions()));
  storage->SetStats(stats::traced_total_buffers,
                    static_cast<int64_t>(evt.total_buffers()));
  storage->SetStats(stats::traced_chunks_discarded,
                    static_cast<int64_t>(evt.chunks_discarded()));
  storage->SetStats(stats::traced_patches_discarded,
                    static_cast<int64_t>(evt.patches_discarded()));
  storage->SetStats(stats::traced_flushes_requested,
                    static_cast<int64_t>(evt.flushes_requested()));
  storage->SetStats(stats::traced_flushes_succeeded,
                    static_cast<int64_t>(evt.flushes_succeeded()));
  storage->SetStats(stats::traced_flushes_failed,
                    static_cast<int64_t>(evt.flushes_failed()));

  if (evt.has_filter_stats()) {
    protos::pbzero::TraceStats::FilterStats::Decoder fstat(evt.filter_stats());
    storage->SetStats(stats::filter_errors,
                      static_cast<int64_t>(fstat.errors()));
    storage->SetStats(stats::filter_input_bytes,
                      static_cast<int64_t>(fstat.input_bytes()));
    storage->SetStats(stats::filter_input_packets,
                      static_cast<int64_t>(fstat.input_packets()));
    storage->SetStats(stats::filter_output_bytes,
                      static_cast<int64_t>(fstat.output_bytes()));
    storage->SetStats(stats::filter_time_taken_ns,
                      static_cast<int64_t>(fstat.time_taken_ns()));
    for (auto [i, it] = std::tuple{0, fstat.bytes_discarded_per_buffer()}; it;
         ++it, ++i) {
      storage->SetIndexedStats(stats::traced_buf_bytes_filtered_out, i,
                               static_cast<int64_t>(*it));
    }
  }

  switch (evt.final_flush_outcome()) {
    case protos::pbzero::TraceStats::FINAL_FLUSH_SUCCEEDED:
      storage->IncrementStats(stats::traced_final_flush_succeeded, 1);
      break;
    case protos::pbzero::TraceStats::FINAL_FLUSH_FAILED:
      storage->IncrementStats(stats::traced_final_flush_failed, 1);
      break;
    case protos::pbzero::TraceStats::FINAL_FLUSH_UNSPECIFIED:
      break;
  }

  int buf_num = 0;
  for (auto it = evt.buffer_stats(); it; ++it, ++buf_num) {
    protos::pbzero::TraceStats::BufferStats::Decoder buf(*it);
    storage->SetIndexedStats(stats::traced_buf_buffer_size, buf_num,
                             static_cast<int64_t>(buf.buffer_size()));
    storage->SetIndexedStats(stats::traced_buf_bytes_written, buf_num,
                             static_cast<int64_t>(buf.bytes_written()));
    storage->SetIndexedStats(stats::traced_buf_bytes_overwritten, buf_num,
                             static_cast<int64_t>(buf.bytes_overwritten()));
    storage->SetIndexedStats(stats::traced_buf_bytes_read, buf_num,
                             static_cast<int64_t>(buf.bytes_read()));
    storage->SetIndexedStats(stats::traced_buf_padding_bytes_written, buf_num,
                             static_cast<int64_t>(buf.padding_bytes_written()));
    storage->SetIndexedStats(stats::traced_buf_padding_bytes_cleared, buf_num,
                             static_cast<int64_t>(buf.padding_bytes_cleared()));
    storage->SetIndexedStats(stats::traced_buf_chunks_written, buf_num,
                             static_cast<int64_t>(buf.chunks_written()));
    storage->SetIndexedStats(stats::traced_buf_chunks_rewritten, buf_num,
                             static_cast<int64_t>(buf.chunks_rewritten()));
    storage->SetIndexedStats(stats::traced_buf_chunks_overwritten, buf_num,
                             static_cast<int64_t>(buf.chunks_overwritten()));
    storage->SetIndexedStats(stats::traced_buf_chunks_discarded, buf_num,
                             static_cast<int64_t>(buf.chunks_discarded()));
    storage->SetIndexedStats(stats::traced_buf_chunks_read, buf_num,
                             static_cast<int64_t>(buf.chunks_read()));
    storage->SetIndexedStats(
        stats::traced_buf_chunks_committed_out_of_order, buf_num,
        static_cast<int64_t>(buf.chunks_committed_out_of_order()));
    storage->SetIndexedStats(stats::traced_buf_write_wrap_count, buf_num,
                             static_cast<int64_t>(buf.write_wrap_count()));
    storage->SetIndexedStats(stats::traced_buf_patches_succeeded, buf_num,
                             static_cast<int64_t>(buf.patches_succeeded()));
    storage->SetIndexedStats(stats::traced_buf_patches_failed, buf_num,
                             static_cast<int64_t>(buf.patches_failed()));
    storage->SetIndexedStats(stats::traced_buf_readaheads_succeeded, buf_num,
                             static_cast<int64_t>(buf.readaheads_succeeded()));
    storage->SetIndexedStats(stats::traced_buf_readaheads_failed, buf_num,
                             static_cast<int64_t>(buf.readaheads_failed()));
    storage->SetIndexedStats(stats::traced_buf_abi_violations, buf_num,
                             static_cast<int64_t>(buf.abi_violations()));
    storage->SetIndexedStats(
        stats::traced_buf_trace_writer_packet_loss, buf_num,
        static_cast<int64_t>(buf.trace_writer_packet_loss()));
  }

  base::FlatHashMap<int32_t, int64_t> data_loss_per_buffer;

  for (auto it = evt.writer_stats(); it; ++it) {
    protos::pbzero::TraceStats::WriterStats::Decoder writer(*it);
    auto* data_loss = packet_sequence_data_loss_.Find(
        static_cast<uint32_t>(writer.sequence_id()));
    if (data_loss) {
      data_loss_per_buffer[static_cast<int32_t>(writer.buffer())] +=
          static_cast<int64_t>(*data_loss);
    }
  }

  for (auto it = data_loss_per_buffer.GetIterator(); it; ++it) {
    storage->SetIndexedStats(stats::traced_buf_sequence_packet_loss, it.key(),
                             it.value());
  }
}

void ProtoTraceReader::NotifyEndOfFile() {}

}  // namespace trace_processor
}  // namespace perfetto
