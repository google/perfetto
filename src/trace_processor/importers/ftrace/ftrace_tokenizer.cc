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

#include "src/trace_processor/importers/ftrace/ftrace_tokenizer.h"

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"

namespace perfetto {
namespace trace_processor {

using protozero::ProtoDecoder;
using protozero::proto_utils::MakeTagVarInt;
using protozero::proto_utils::ParseVarInt;

using protos::pbzero::BuiltinClock;
using protos::pbzero::FtraceClock;
using protos::pbzero::FtraceEventBundle;

namespace {

static constexpr uint32_t kFtraceGlobalClockIdForOldKernels = 64;

PERFETTO_ALWAYS_INLINE base::StatusOr<int64_t> ResolveTraceTime(
    TraceProcessorContext* context,
    ClockTracker::ClockId clock_id,
    int64_t ts) {
  // On most traces (i.e. P+), the clock should be BOOTTIME.
  if (PERFETTO_LIKELY(clock_id == BuiltinClock::BUILTIN_CLOCK_BOOTTIME))
    return ts;
  return context->clock_tracker->ToTraceTime(clock_id, ts);
}

// Fast path for parsing the event id of an ftrace event.
// Speculate on the fact that, if the timestamp was found, the common pid
// will appear immediately after and the event id immediately after that.
uint64_t TryFastParseFtraceEventId(const uint8_t* start, const uint8_t* end) {
  constexpr auto kPidFieldNumber = protos::pbzero::FtraceEvent::kPidFieldNumber;
  constexpr auto kPidFieldTag = MakeTagVarInt(kPidFieldNumber);

  // If the next byte is not the common pid's tag, just skip the field.
  constexpr uint32_t kMaxPidLength = 5;
  if (PERFETTO_UNLIKELY(static_cast<uint32_t>(end - start) <= kMaxPidLength ||
                        start[0] != kPidFieldTag)) {
    return 0;
  }

  // Skip the common pid.
  uint64_t common_pid = 0;
  const uint8_t* common_pid_end = ParseVarInt(start + 1, end, &common_pid);
  if (PERFETTO_UNLIKELY(common_pid_end == start + 1)) {
    return 0;
  }

  // Read the next varint: this should be the event id tag.
  uint64_t event_tag = 0;
  const uint8_t* event_id_end = ParseVarInt(common_pid_end, end, &event_tag);
  if (event_id_end == common_pid_end) {
    return 0;
  }

  constexpr uint8_t kFieldTypeNumBits = 3;
  constexpr uint64_t kFieldTypeMask =
      (1 << kFieldTypeNumBits) - 1;  // 0000 0111;

  // The event wire type should be length delimited.
  auto wire_type = static_cast<protozero::proto_utils::ProtoWireType>(
      event_tag & kFieldTypeMask);
  if (wire_type != protozero::proto_utils::ProtoWireType::kLengthDelimited) {
    return 0;
  }
  return event_tag >> kFieldTypeNumBits;
}

}  // namespace

PERFETTO_ALWAYS_INLINE
base::Status FtraceTokenizer::TokenizeFtraceBundle(
    TraceBlobView bundle,
    PacketSequenceState* state,
    uint32_t packet_sequence_id) {
  protos::pbzero::FtraceEventBundle::Decoder decoder(bundle.data(),
                                                     bundle.length());

  if (PERFETTO_UNLIKELY(!decoder.has_cpu())) {
    PERFETTO_ELOG("CPU field not found in FtraceEventBundle");
    context_->storage->IncrementStats(stats::ftrace_bundle_tokenizer_errors);
    return base::OkStatus();
  }

  uint32_t cpu = decoder.cpu();
  static constexpr uint32_t kMaxCpuCount = 1024;
  if (PERFETTO_UNLIKELY(cpu >= kMaxCpuCount)) {
    return base::ErrStatus(
        "CPU %u is greater than maximum allowed of %u. This is likely because "
        "of trace corruption",
        cpu, kMaxCpuCount);
  }

  if (PERFETTO_UNLIKELY(decoder.lost_events())) {
    // If set, it means that the kernel overwrote an unspecified number of
    // events since our last read from the per-cpu buffer.
    context_->storage->SetIndexedStats(stats::ftrace_cpu_has_data_loss,
                                       static_cast<int>(cpu), 1);
  }

  ClockTracker::ClockId clock_id;
  switch (decoder.ftrace_clock()) {
    case FtraceClock::FTRACE_CLOCK_UNSPECIFIED:
      clock_id = BuiltinClock::BUILTIN_CLOCK_BOOTTIME;
      break;
    case FtraceClock::FTRACE_CLOCK_GLOBAL:
      clock_id = ClockTracker::SeqenceToGlobalClock(
          packet_sequence_id, kFtraceGlobalClockIdForOldKernels);
      break;
    case FtraceClock::FTRACE_CLOCK_MONO_RAW:
      clock_id = BuiltinClock::BUILTIN_CLOCK_MONOTONIC_RAW;
      break;
    case FtraceClock::FTRACE_CLOCK_LOCAL:
      return base::ErrStatus("Unable to parse ftrace packets with local clock");
    default:
      return base::ErrStatus(
          "Unable to parse ftrace packets with unknown clock");
  }

  if (decoder.has_ftrace_timestamp()) {
    PERFETTO_DCHECK(clock_id != BuiltinClock::BUILTIN_CLOCK_BOOTTIME);
    HandleFtraceClockSnapshot(decoder.ftrace_timestamp(),
                              decoder.boot_timestamp(), packet_sequence_id);
  }

  if (decoder.has_compact_sched()) {
    TokenizeFtraceCompactSched(cpu, clock_id, decoder.compact_sched());
  }

  for (auto it = decoder.event(); it; ++it) {
    TokenizeFtraceEvent(cpu, clock_id, bundle.slice(it->data(), it->size()),
                        state);
  }
  return base::OkStatus();
}

PERFETTO_ALWAYS_INLINE
void FtraceTokenizer::TokenizeFtraceEvent(uint32_t cpu,
                                          ClockTracker::ClockId clock_id,
                                          TraceBlobView event,
                                          PacketSequenceState* state) {
  constexpr auto kTimestampFieldNumber =
      protos::pbzero::FtraceEvent::kTimestampFieldNumber;
  constexpr auto kTimestampFieldTag = MakeTagVarInt(kTimestampFieldNumber);

  const uint8_t* data = event.data();
  const size_t length = event.length();

  // Speculate on the following sequence of varints
  //  - timestamp tag
  //  - timestamp (64 bit)
  //  - common pid tag
  //  - common pid (32 bit)
  //  - event tag
  uint64_t raw_timestamp = 0;
  bool timestamp_found = false;
  uint64_t event_id = 0;
  if (PERFETTO_LIKELY(length > 10 && data[0] == kTimestampFieldTag)) {
    // Fastpath.
    const uint8_t* ts_end = ParseVarInt(data + 1, data + 11, &raw_timestamp);
    timestamp_found = ts_end != data + 1;
    if (PERFETTO_LIKELY(timestamp_found)) {
      event_id = TryFastParseFtraceEventId(ts_end, data + length);
    }
  }

  // Slowpath for finding the timestamp.
  if (PERFETTO_UNLIKELY(!timestamp_found)) {
    ProtoDecoder decoder(data, length);
    if (auto ts_field = decoder.FindField(kTimestampFieldNumber)) {
      timestamp_found = true;
      raw_timestamp = ts_field.as_uint64();
    }
    if (PERFETTO_UNLIKELY(!timestamp_found)) {
      context_->storage->IncrementStats(stats::ftrace_bundle_tokenizer_errors);
      return;
    }
  }

  // Slowpath for finding the event id.
  if (PERFETTO_UNLIKELY(event_id == 0)) {
    ProtoDecoder decoder(data, length);
    for (auto f = decoder.ReadField(); f.valid(); f = decoder.ReadField()) {
      // Find the first length-delimited tag as this corresponds to the ftrace
      // event.
      if (f.type() == protozero::proto_utils::ProtoWireType::kLengthDelimited) {
        event_id = f.id();
        break;
      }
    }
    if (PERFETTO_UNLIKELY(event_id == 0)) {
      context_->storage->IncrementStats(stats::ftrace_missing_event_id);
      return;
    }
  }

  if (PERFETTO_UNLIKELY(
          event_id == protos::pbzero::FtraceEvent::kGpuWorkPeriodFieldNumber)) {
    TokenizeFtraceGpuWorkPeriod(cpu, std::move(event), state);
    return;
  }

  int64_t int64_timestamp = static_cast<int64_t>(raw_timestamp);
  base::StatusOr<int64_t> timestamp =
      ResolveTraceTime(context_, clock_id, int64_timestamp);

  // ClockTracker will increment some error stats if it failed to convert the
  // timestamp so just return.
  if (!timestamp.ok()) {
    DlogWithLimit(timestamp.status());
    return;
  }

  context_->sorter->PushFtraceEvent(cpu, *timestamp, std::move(event),
                                    state->current_generation());
}

PERFETTO_ALWAYS_INLINE
void FtraceTokenizer::TokenizeFtraceCompactSched(uint32_t cpu,
                                                 ClockTracker::ClockId clock_id,
                                                 protozero::ConstBytes packet) {
  FtraceEventBundle::CompactSched::Decoder compact_sched(packet);

  // Build the interning table for comm fields.
  std::vector<StringId> string_table;
  string_table.reserve(512);
  for (auto it = compact_sched.intern_table(); it; it++) {
    StringId value = context_->storage->InternString(*it);
    string_table.push_back(value);
  }

  TokenizeFtraceCompactSchedSwitch(cpu, clock_id, compact_sched, string_table);
  TokenizeFtraceCompactSchedWaking(cpu, clock_id, compact_sched, string_table);
}

void FtraceTokenizer::TokenizeFtraceCompactSchedSwitch(
    uint32_t cpu,
    ClockTracker::ClockId clock_id,
    const FtraceEventBundle::CompactSched::Decoder& compact,
    const std::vector<StringId>& string_table) {
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

    base::StatusOr<int64_t> timestamp =
        ResolveTraceTime(context_, clock_id, event_timestamp);
    if (!timestamp.ok()) {
      DlogWithLimit(timestamp.status());
      return;
    }
    context_->sorter->PushInlineFtraceEvent(cpu, *timestamp, event);
  }

  // Check that all packed buffers were decoded correctly, and fully.
  bool sizes_match =
      !timestamp_it && !pstate_it && !npid_it && !nprio_it && !comm_it;
  if (parse_error || !sizes_match)
    context_->storage->IncrementStats(stats::compact_sched_has_parse_errors);
}

void FtraceTokenizer::TokenizeFtraceCompactSchedWaking(
    uint32_t cpu,
    ClockTracker::ClockId clock_id,
    const FtraceEventBundle::CompactSched::Decoder& compact,
    const std::vector<StringId>& string_table) {
  // Accumulator for timestamp deltas.
  int64_t timestamp_acc = 0;

  // The events' fields are stored in a structure-of-arrays style, using packed
  // repeated fields. Walk each repeated field in step to recover individual
  // events.
  bool parse_error = false;
  auto timestamp_it = compact.waking_timestamp(&parse_error);
  auto pid_it = compact.waking_pid(&parse_error);
  auto tcpu_it = compact.waking_target_cpu(&parse_error);
  auto prio_it = compact.waking_prio(&parse_error);
  auto comm_it = compact.waking_comm_index(&parse_error);
  auto common_flags_it = compact.waking_common_flags(&parse_error);

  for (; timestamp_it && pid_it && tcpu_it && prio_it && comm_it;
       ++timestamp_it, ++pid_it, ++tcpu_it, ++prio_it, ++comm_it) {
    InlineSchedWaking event{};

    // delta-encoded timestamp
    timestamp_acc += static_cast<int64_t>(*timestamp_it);
    int64_t event_timestamp = timestamp_acc;

    // index into the interned string table
    PERFETTO_DCHECK(*comm_it < string_table.size());
    event.comm = string_table[*comm_it];

    event.pid = *pid_it;
    event.target_cpu = static_cast<uint16_t>(*tcpu_it);
    event.prio = static_cast<uint16_t>(*prio_it);

    if (common_flags_it) {
      event.common_flags = static_cast<uint16_t>(*common_flags_it);
      common_flags_it++;
    }

    base::StatusOr<int64_t> timestamp =
        ResolveTraceTime(context_, clock_id, event_timestamp);
    if (!timestamp.ok()) {
      DlogWithLimit(timestamp.status());
      return;
    }
    context_->sorter->PushInlineFtraceEvent(cpu, *timestamp, event);
  }

  // Check that all packed buffers were decoded correctly, and fully.
  bool sizes_match =
      !timestamp_it && !pid_it && !tcpu_it && !prio_it && !comm_it;
  if (parse_error || !sizes_match)
    context_->storage->IncrementStats(stats::compact_sched_has_parse_errors);
}

void FtraceTokenizer::HandleFtraceClockSnapshot(int64_t ftrace_ts,
                                                int64_t boot_ts,
                                                uint32_t packet_sequence_id) {
  // If we've already seen a snapshot at this timestamp, don't unnecessarily
  // add another entry to the clock tracker.
  if (latest_ftrace_clock_snapshot_ts_ == ftrace_ts)
    return;
  latest_ftrace_clock_snapshot_ts_ = ftrace_ts;

  ClockTracker::ClockId global_id = ClockTracker::SeqenceToGlobalClock(
      packet_sequence_id, kFtraceGlobalClockIdForOldKernels);
  context_->clock_tracker->AddSnapshot(
      {ClockTracker::ClockTimestamp(global_id, ftrace_ts),
       ClockTracker::ClockTimestamp(BuiltinClock::BUILTIN_CLOCK_BOOTTIME,
                                    boot_ts)});
}

void FtraceTokenizer::TokenizeFtraceGpuWorkPeriod(uint32_t cpu,
                                                  TraceBlobView event,
                                                  PacketSequenceState* state) {
  // Special handling of valid gpu_work_period tracepoint events which contain
  // timestamp values for the GPU time period nested inside the event data.
  const uint8_t* data = event.data();
  const size_t length = event.length();

  ProtoDecoder decoder(data, length);
  auto ts_field =
      decoder.FindField(protos::pbzero::FtraceEvent::kGpuWorkPeriodFieldNumber);
  if (!ts_field.valid()) {
    context_->storage->IncrementStats(stats::ftrace_bundle_tokenizer_errors);
    return;
  }

  protos::pbzero::GpuWorkPeriodFtraceEvent::Decoder gpu_work_event(
      ts_field.data(), ts_field.size());
  if (!gpu_work_event.has_start_time_ns()) {
    context_->storage->IncrementStats(stats::ftrace_bundle_tokenizer_errors);
    return;
  }
  uint64_t raw_timestamp = gpu_work_event.start_time_ns();

  // Enforce clock type for the event data to be CLOCK_MONOTONIC_RAW
  // as specified, to calculate the timestamp correctly.
  int64_t int64_timestamp = static_cast<int64_t>(raw_timestamp);
  base::StatusOr<int64_t> timestamp = ResolveTraceTime(
      context_, BuiltinClock::BUILTIN_CLOCK_MONOTONIC_RAW, int64_timestamp);

  // ClockTracker will increment some error stats if it failed to convert the
  // timestamp so just return.
  if (!timestamp.ok()) {
    DlogWithLimit(timestamp.status());
    return;
  }

  context_->sorter->PushFtraceEvent(cpu, *timestamp, std::move(event),
                                    state->current_generation());
}

}  // namespace trace_processor
}  // namespace perfetto
