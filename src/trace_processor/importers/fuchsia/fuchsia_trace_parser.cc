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

#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_utils.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/sched_tables_py.h"

namespace perfetto::trace_processor {

namespace {

// Record Types
constexpr uint32_t kEvent = 4;
constexpr uint32_t kSchedulerEvent = 8;

constexpr uint32_t kSchedulerEventLegacyContextSwitch = 0;
constexpr uint32_t kSchedulerEventContextSwitch = 1;
constexpr uint32_t kSchedulerEventThreadWakeup = 2;

// Event Types
constexpr uint32_t kInstant = 0;
constexpr uint32_t kCounter = 1;
constexpr uint32_t kDurationBegin = 2;
constexpr uint32_t kDurationEnd = 3;
constexpr uint32_t kDurationComplete = 4;
constexpr uint32_t kAsyncBegin = 5;
constexpr uint32_t kAsyncInstant = 6;
constexpr uint32_t kAsyncEnd = 7;
constexpr uint32_t kFlowBegin = 8;
constexpr uint32_t kFlowStep = 9;
constexpr uint32_t kFlowEnd = 10;

// Argument Types
constexpr uint32_t kNull = 0;
constexpr uint32_t kInt32 = 1;
constexpr uint32_t kUint32 = 2;
constexpr uint32_t kInt64 = 3;
constexpr uint32_t kUint64 = 4;
constexpr uint32_t kDouble = 5;
constexpr uint32_t kString = 6;
constexpr uint32_t kPointer = 7;
constexpr uint32_t kKoid = 8;
constexpr uint32_t kBool = 9;

// Thread states
constexpr uint32_t kThreadNew = 0;
constexpr uint32_t kThreadRunning = 1;
constexpr uint32_t kThreadSuspended = 2;
constexpr uint32_t kThreadBlocked = 3;
constexpr uint32_t kThreadDying = 4;
constexpr uint32_t kThreadDead = 5;

constexpr int32_t kIdleWeight = std::numeric_limits<int32_t>::min();

// CPU idle states (matching Linux/Android conventions: 0 is idle, 0xFFFFFFFF is
// active/exit idle)
constexpr double kCpuIdleState = 0.0;
constexpr double kCpuActiveState =
    static_cast<double>(std::numeric_limits<uint32_t>::max());

// Trace contract for CPU frequency counter events emitted by Fuchsia CPU
// drivers, aligned with the Linux ftrace "power/cpu_frequency" event: the
// counter record carries the logical core index in the "cpu" argument and the
// frequency in kHz in the "value" argument.
constexpr char kCpuFrequencyCategory[] = "power";
constexpr char kCpuFrequencyEventName[] = "cpu_frequency";
constexpr char kCpuFrequencyCpuArg[] = "cpu";
constexpr char kCpuFrequencyValueArg[] = "value";

constexpr auto kCounterBlueprint = tracks::CounterBlueprint(
    "fuchsia_counter",
    tracks::UnknownUnitBlueprint(),
    tracks::DimensionBlueprints(tracks::kProcessDimensionBlueprint,
                                tracks::kNameFromTraceDimensionBlueprint),
    tracks::DynamicNameBlueprint());

// Returns the numeric value carried by |value| as a double, or std::nullopt if
// the argument does not hold a numeric type. Range and finiteness checks are
// left to the caller.
std::optional<double> ArgValueToDouble(
    const fuchsia_trace_utils::ArgValue& value) {
  switch (value.Type()) {
    case fuchsia_trace_utils::ArgValue::kInt32:
      return static_cast<double>(value.Int32());
    case fuchsia_trace_utils::ArgValue::kUint32:
      return static_cast<double>(value.Uint32());
    case fuchsia_trace_utils::ArgValue::kInt64:
      return static_cast<double>(value.Int64());
    case fuchsia_trace_utils::ArgValue::kUint64:
      return static_cast<double>(value.Uint64());
    case fuchsia_trace_utils::ArgValue::kDouble:
      return value.Double();
    case fuchsia_trace_utils::ArgValue::kNull:
    case fuchsia_trace_utils::ArgValue::kString:
    case fuchsia_trace_utils::ArgValue::kPointer:
    case fuchsia_trace_utils::ArgValue::kKoid:
    case fuchsia_trace_utils::ArgValue::kBool:
    case fuchsia_trace_utils::ArgValue::kUnknown:
      return std::nullopt;
  }
  PERFETTO_FATAL("For GCC");
}

// Returns the logical CPU core index carried by |value|, or std::nullopt if the
// value is not a valid core index. Negative and out of range indices
// (>= CpuTracker::kMaxCpusPerMachine) are rejected, as are non-integer and
// non-numeric argument types.
std::optional<uint32_t> ExtractCpuCoreIndex(
    const fuchsia_trace_utils::ArgValue& value) {
  if (value.Type() == fuchsia_trace_utils::ArgValue::kDouble) {
    return std::nullopt;
  }
  // Integers up to 2^53 round-trip exactly through a double, so every value in
  // the valid core index range is compared and converted without loss below.
  std::optional<double> core_index = ArgValueToDouble(value);
  if (!core_index.has_value() || *core_index < 0 ||
      *core_index >= CpuTracker::kMaxCpusPerMachine) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(*core_index);
}

// Returns the frequency in kHz carried by |value|, or std::nullopt if the
// value is not a valid frequency. Negative and non-finite (NaN, infinity)
// frequencies are rejected, as are non-numeric argument types.
std::optional<double> ExtractFrequencyKhz(
    const fuchsia_trace_utils::ArgValue& value) {
  std::optional<double> frequency_khz = ArgValueToDouble(value);
  if (!frequency_khz.has_value() || !std::isfinite(*frequency_khz) ||
      *frequency_khz < 0) {
    return std::nullopt;
  }
  return frequency_khz;
}

}  // namespace

FuchsiaTraceParser::FuchsiaTraceParser(TraceProcessorContext* context)
    : context_(context),
      weight_id_(context->storage->InternString("weight")),
      waker_id_(context->storage->InternString("waker")),
      incoming_weight_id_(context->storage->InternString("incoming_weight")),
      outgoing_weight_id_(context->storage->InternString("outgoing_weight")),
      cpu_frequency_category_id_(
          context->storage->InternString(kCpuFrequencyCategory)),
      cpu_frequency_name_id_(
          context->storage->InternString(kCpuFrequencyEventName)),
      cpu_frequency_cpu_arg_id_(
          context->storage->InternString(kCpuFrequencyCpuArg)),
      cpu_frequency_value_arg_id_(
          context->storage->InternString(kCpuFrequencyValueArg)),
      running_string_id_(context->storage->InternString("Running")),
      runnable_string_id_(context->storage->InternString("R")),
      waking_string_id_(context->storage->InternString("W")),
      blocked_string_id_(context->storage->InternString("S")),
      suspended_string_id_(context->storage->InternString("T")),
      exit_dying_string_id_(context->storage->InternString("Z")),
      exit_dead_string_id_(context->storage->InternString("X")) {}

FuchsiaTraceParser::~FuchsiaTraceParser() = default;

std::optional<std::vector<FuchsiaTraceParser::Arg>>
FuchsiaTraceParser::ParseArgs(
    fuchsia_trace_utils::RecordCursor& cursor,
    uint32_t n_args,
    std::function<StringId(base::StringView string)> intern_string,
    std::function<StringId(uint32_t index)> get_string) {
  std::vector<Arg> args;
  for (uint32_t i = 0; i < n_args; i++) {
    size_t arg_base = cursor.WordIndex();
    uint64_t arg_header;
    if (!cursor.ReadUint64(&arg_header)) {
      return std::nullopt;
    }
    uint32_t arg_type =
        fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 0, 3);
    uint32_t arg_size_words =
        fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 4, 15);
    uint32_t arg_name_ref =
        fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 16, 31);
    Arg arg;
    if (fuchsia_trace_utils::IsInlineString(arg_name_ref)) {
      base::StringView arg_name_view;
      if (!cursor.ReadInlineString(arg_name_ref, &arg_name_view)) {
        return std::nullopt;
      }
      arg.name = intern_string(arg_name_view);
    } else {
      arg.name = get_string(arg_name_ref);
    }

    switch (arg_type) {
      case kNull:
        arg.value = fuchsia_trace_utils::ArgValue::Null();
        break;
      case kInt32:
        arg.value = fuchsia_trace_utils::ArgValue::Int32(
            fuchsia_trace_utils::ReadField<int32_t>(arg_header, 32, 63));
        break;
      case kUint32:
        arg.value = fuchsia_trace_utils::ArgValue::Uint32(
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 32, 63));
        break;
      case kInt64: {
        int64_t value;
        if (!cursor.ReadInt64(&value)) {
          return std::nullopt;
        }
        arg.value = fuchsia_trace_utils::ArgValue::Int64(value);
        break;
      }
      case kUint64: {
        uint64_t value;
        if (!cursor.ReadUint64(&value)) {
          return std::nullopt;
        }
        arg.value = fuchsia_trace_utils::ArgValue::Uint64(value);
        break;
      }
      case kDouble: {
        double value;
        if (!cursor.ReadDouble(&value)) {
          return std::nullopt;
        }
        arg.value = fuchsia_trace_utils::ArgValue::Double(value);
        break;
      }
      case kString: {
        uint32_t arg_value_ref =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 32, 47);
        StringId value;
        if (fuchsia_trace_utils::IsInlineString(arg_value_ref)) {
          base::StringView arg_value_view;
          if (!cursor.ReadInlineString(arg_value_ref, &arg_value_view)) {
            return std::nullopt;
          }
          value = intern_string(arg_value_view);
        } else {
          value = get_string(arg_value_ref);
        }
        arg.value = fuchsia_trace_utils::ArgValue::String(value);
        break;
      }
      case kPointer: {
        uint64_t value;
        if (!cursor.ReadUint64(&value)) {
          return std::nullopt;
        }
        arg.value = fuchsia_trace_utils::ArgValue::Pointer(value);
        break;
      }
      case kKoid: {
        uint64_t value;
        if (!cursor.ReadUint64(&value)) {
          return std::nullopt;
        }
        arg.value = fuchsia_trace_utils::ArgValue::Koid(value);
        break;
      }
      case kBool: {
        arg.value = fuchsia_trace_utils::ArgValue::Bool(
            fuchsia_trace_utils::ReadField<bool>(arg_header, 32, 63));
        break;
      }
      default:
        arg.value = fuchsia_trace_utils::ArgValue::Unknown();
        break;
    }

    args.push_back(arg);
    cursor.SetWordIndex(arg_base + arg_size_words);
  }

  return {std::move(args)};
}

void FuchsiaTraceParser::Parse(int64_t, FuchsiaRecord fr) {
  // The timestamp is also present in the record, so we'll ignore the one
  // passed as an argument.
  fuchsia_trace_utils::RecordCursor cursor(fr.record_view()->data(),
                                           fr.record_view()->length());
  ProcessTracker* procs = context_->process_tracker.get();
  SliceTracker* slices = context_->slice_tracker.get();

  // Read arguments
  const auto intern_string = [this](base::StringView string) {
    return context_->storage->InternString(string);
  };
  const auto get_string = [&fr](uint32_t index) { return fr.GetString(index); };

  uint64_t header;
  if (!cursor.ReadUint64(&header)) {
    context_->stats_tracker->IncrementStats(stats::fuchsia_record_read_error);
    return;
  }
  auto record_type = fuchsia_trace_utils::ReadField<uint32_t>(header, 0, 3);
  switch (record_type) {
    case kEvent: {
      auto event_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
      auto n_args = fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 23);
      auto thread_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 31);
      auto cat_ref = fuchsia_trace_utils::ReadField<uint32_t>(header, 32, 47);
      auto name_ref = fuchsia_trace_utils::ReadField<uint32_t>(header, 48, 63);

      int64_t ts;
      if (!cursor.ReadTimestamp(fr.get_ticks_per_second(), &ts)) {
        context_->stats_tracker->IncrementStats(
            stats::fuchsia_record_read_error);
        return;
      }
      FuchsiaThreadInfo tinfo;
      if (fuchsia_trace_utils::IsInlineThread(thread_ref)) {
        if (!cursor.ReadInlineThread(&tinfo)) {
          context_->stats_tracker->IncrementStats(
              stats::fuchsia_record_read_error);
          return;
        }
      } else {
        tinfo = fr.GetThread(thread_ref);
      }
      StringId cat;
      if (fuchsia_trace_utils::IsInlineString(cat_ref)) {
        base::StringView cat_string_view;
        if (!cursor.ReadInlineString(cat_ref, &cat_string_view)) {
          context_->stats_tracker->IncrementStats(
              stats::fuchsia_record_read_error);
          return;
        }
        cat = context_->storage->InternString(cat_string_view);
      } else {
        cat = fr.GetString(cat_ref);
      }
      StringId name;
      if (fuchsia_trace_utils::IsInlineString(name_ref)) {
        base::StringView name_string_view;
        if (!cursor.ReadInlineString(name_ref, &name_string_view)) {
          context_->stats_tracker->IncrementStats(
              stats::fuchsia_record_read_error);
          return;
        }
        name = context_->storage->InternString(name_string_view);
      } else {
        name = fr.GetString(name_ref);
      }

      auto maybe_args = FuchsiaTraceParser::ParseArgs(
          cursor, n_args, intern_string, get_string);
      if (!maybe_args.has_value()) {
        context_->stats_tracker->IncrementStats(
            stats::fuchsia_record_read_error);
        return;
      }

      auto insert_args =
          [this, args = *maybe_args](ArgsTracker::BoundInserter* inserter) {
            for (const Arg& arg : args) {
              inserter->AddArg(
                  arg.name, arg.name,
                  arg.value.ToStorageVariadic(context_->storage.get()));
            }
          };

      switch (event_type) {
        case kInstant: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          slices->Scoped(ts, track_id, cat, name, 0, std::move(insert_args));
          break;
        }
        case kCounter: {
          uint64_t counter_id;
          if (!cursor.ReadUint64(&counter_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }

          // CPU frequency counters go to the standard per-core cpufreq tracks
          // instead of to a generic process counter track.
          if (cat == cpu_frequency_category_id_ &&
              name == cpu_frequency_name_id_) {
            ParseCpuFrequencyCounter(ts, *maybe_args);
            break;
          }

          UniquePid upid =
              procs->GetOrCreateProcess(static_cast<uint32_t>(tinfo.pid));
          std::string name_str =
              context_->storage->GetString(name).ToStdString();

          // Note: In the Fuchsia trace format, counter values are stored
          // in the arguments for the record, with the data series defined
          // by both the record name and the argument name. In Perfetto,
          // counters only have one name, so we combine both names into
          // one here.
          for (const Arg& arg : *maybe_args) {
            std::optional<double> counter_value = ArgValueToDouble(arg.value);
            if (!counter_value.has_value()) {
              context_->stats_tracker->IncrementStats(
                  stats::fuchsia_non_numeric_counters);
              continue;
            }
            std::string counter_name_str = name_str + ":";
            counter_name_str +=
                context_->storage->GetString(arg.name).ToStdString();
            counter_name_str += ":" + std::to_string(counter_id);
            base::StringView counter_name_str_view(counter_name_str);
            StringId counter_name_id =
                context_->storage->InternString(counter_name_str_view);
            TrackId track = context_->track_tracker->InternTrack(
                kCounterBlueprint,
                tracks::Dimensions(upid, counter_name_str_view),
                tracks::DynamicName(counter_name_id));
            context_->event_tracker->PushCounter(ts, *counter_value, track);
          }
          break;
        }
        case kDurationBegin: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          slices->Begin(ts, track_id, cat, name, std::move(insert_args));
          break;
        }
        case kDurationEnd: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          // TODO(b/131181693): |cat| and |name| are not passed here so
          // that if two slices end at the same timestep, the slices get
          // closed in the correct order regardless of which end event is
          // processed first.
          slices->End(ts, track_id, {}, {}, std::move(insert_args));
          break;
        }
        case kDurationComplete: {
          int64_t end_ts;
          if (!cursor.ReadTimestamp(fr.get_ticks_per_second(), &end_ts)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          int64_t duration = end_ts - ts;
          if (duration < 0) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_timestamp_overflow);
            return;
          }
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          slices->Scoped(ts, track_id, cat, name, duration,
                         std::move(insert_args));
          break;
        }
        case kAsyncBegin: {
          int64_t correlation_id;
          if (!cursor.ReadInt64(&correlation_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          UniquePid upid =
              procs->GetOrCreateProcess(static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_compressor->InternLegacyAsyncTrack(
              name, upid, correlation_id, false, kNullStringId,
              TrackCompressor::AsyncSliceType::kBegin);
          slices->Begin(ts, track_id, cat, name, std::move(insert_args));
          break;
        }
        case kAsyncInstant: {
          int64_t correlation_id;
          if (!cursor.ReadInt64(&correlation_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          UniquePid upid =
              procs->GetOrCreateProcess(static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_compressor->InternLegacyAsyncTrack(
              name, upid, correlation_id, false, kNullStringId,
              TrackCompressor::AsyncSliceType::kInstant);
          slices->Scoped(ts, track_id, cat, name, 0, std::move(insert_args));
          break;
        }
        case kAsyncEnd: {
          int64_t correlation_id;
          if (!cursor.ReadInt64(&correlation_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          UniquePid upid =
              procs->GetOrCreateProcess(static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_compressor->InternLegacyAsyncTrack(
              name, upid, correlation_id, false, kNullStringId,
              TrackCompressor::AsyncSliceType::kEnd);
          slices->End(ts, track_id, cat, name, std::move(insert_args));
          break;
        }
        case kFlowBegin: {
          uint64_t correlation_id;
          if (!cursor.ReadUint64(&correlation_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          auto opt_resolved =
              GetGlobalFlowId(static_cast<uint32_t>(tinfo.pid), correlation_id,
                              /* step_or_end = */ false);
          if (opt_resolved) {
            context_->flow_tracker->Begin(track_id, opt_resolved->global_id);
          }
          break;
        }
        case kFlowStep: {
          uint64_t correlation_id;
          if (!cursor.ReadUint64(&correlation_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          auto opt_resolved =
              GetGlobalFlowId(static_cast<uint32_t>(tinfo.pid), correlation_id,
                              /* step_or_end = */ true);
          if (opt_resolved) {
            context_->flow_tracker->Step(track_id, opt_resolved->global_id);
          }
          break;
        }
        case kFlowEnd: {
          uint64_t correlation_id;
          if (!cursor.ReadUint64(&correlation_id)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
          auto opt_resolved =
              GetGlobalFlowId(static_cast<uint32_t>(tinfo.pid), correlation_id,
                              /* step_or_end = */ true);
          if (opt_resolved) {
            context_->flow_tracker->End(track_id, opt_resolved->global_id,
                                        /* bind_enclosing_slice = */ true,
                                        /* close_flow = */ true);
            fuchsia_active_flows_.Erase(opt_resolved->matched_scoped);
            fuchsia_global_flows_.Erase(
                opt_resolved->matched_scoped.correlation_id);
          }
          break;
        }
      }
      break;
    }
    case kSchedulerEvent: {
      auto event_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 60, 63);
      switch (event_type) {
        case kSchedulerEventLegacyContextSwitch: {
          auto cpu = fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
          auto outgoing_state =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 27);
          auto outgoing_thread_ref =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 28, 35);
          auto incoming_thread_ref =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 36, 43);
          auto outgoing_priority =
              fuchsia_trace_utils::ReadField<int32_t>(header, 44, 51);
          auto incoming_priority =
              fuchsia_trace_utils::ReadField<int32_t>(header, 52, 59);

          int64_t ts;
          if (!cursor.ReadTimestamp(fr.get_ticks_per_second(), &ts)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          if (ts < 0) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_timestamp_overflow);
            return;
          }

          FuchsiaThreadInfo outgoing_thread_info;
          if (fuchsia_trace_utils::IsInlineThread(outgoing_thread_ref)) {
            if (!cursor.ReadInlineThread(&outgoing_thread_info)) {
              context_->stats_tracker->IncrementStats(
                  stats::fuchsia_record_read_error);
              return;
            }
          } else {
            outgoing_thread_info = fr.GetThread(outgoing_thread_ref);
          }
          Thread& outgoing_thread = GetThread(outgoing_thread_info.tid);

          FuchsiaThreadInfo incoming_thread_info;
          if (fuchsia_trace_utils::IsInlineThread(incoming_thread_ref)) {
            if (!cursor.ReadInlineThread(&incoming_thread_info)) {
              context_->stats_tracker->IncrementStats(
                  stats::fuchsia_record_read_error);
              return;
            }
          } else {
            incoming_thread_info = fr.GetThread(incoming_thread_ref);
          }
          Thread& incoming_thread = GetThread(incoming_thread_info.tid);

          // Idle threads are identified by pid == 0 and prio == 0.
          const bool incoming_is_idle =
              incoming_thread.info.pid == 0 && incoming_priority == 0;
          const bool outgoing_is_idle =
              outgoing_thread.info.pid == 0 && outgoing_priority == 0;

          // Handle switching away from the currently running thread.
          if (!outgoing_is_idle) {
            SwitchFrom(&outgoing_thread, ts, cpu, outgoing_state);
          }

          // Handle switching to the new currently running thread.
          if (!incoming_is_idle) {
            SwitchTo(&incoming_thread, ts, cpu, incoming_priority);
          }

          MaybePushCpuIdleTransition(ts, cpu, outgoing_is_idle,
                                     incoming_is_idle);
          break;
        }
        case kSchedulerEventContextSwitch: {
          const auto argument_count =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
          const auto cpu =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 35);
          const auto outgoing_state =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 36, 39);

          int64_t ts;
          if (!cursor.ReadTimestamp(fr.get_ticks_per_second(), &ts)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          if (ts < 0) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_timestamp_overflow);
            return;
          }

          uint64_t outgoing_tid;
          if (!cursor.ReadUint64(&outgoing_tid)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          Thread& outgoing_thread = GetThread(outgoing_tid);

          uint64_t incoming_tid;
          if (!cursor.ReadUint64(&incoming_tid)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          Thread& incoming_thread = GetThread(incoming_tid);

          auto maybe_args = FuchsiaTraceParser::ParseArgs(
              cursor, argument_count, intern_string, get_string);
          if (!maybe_args.has_value()) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }

          int32_t incoming_weight = 0;
          int32_t outgoing_weight = 0;

          for (const auto& arg : *maybe_args) {
            if (arg.name == incoming_weight_id_) {
              if (arg.value.Type() !=
                  fuchsia_trace_utils::ArgValue::ArgType::kInt32) {
                context_->stats_tracker->IncrementStats(
                    stats::fuchsia_invalid_event_arg_type);
                return;
              }
              incoming_weight = arg.value.Int32();
            } else if (arg.name == outgoing_weight_id_) {
              if (arg.value.Type() !=
                  fuchsia_trace_utils::ArgValue::ArgType::kInt32) {
                context_->stats_tracker->IncrementStats(
                    stats::fuchsia_invalid_event_arg_type);
                return;
              }
              outgoing_weight = arg.value.Int32();
            }
          }

          const bool incoming_is_idle = incoming_weight == kIdleWeight;
          const bool outgoing_is_idle = outgoing_weight == kIdleWeight;

          // Handle switching away from the currently running thread.
          if (!outgoing_is_idle) {
            SwitchFrom(&outgoing_thread, ts, cpu, outgoing_state);
          }

          // Handle switching to the new currently running thread.
          if (!incoming_is_idle) {
            SwitchTo(&incoming_thread, ts, cpu, incoming_weight);
          }

          MaybePushCpuIdleTransition(ts, cpu, outgoing_is_idle,
                                     incoming_is_idle);
          break;
        }
        case kSchedulerEventThreadWakeup: {
          const auto argument_count =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
          const auto cpu =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 35);

          int64_t ts;
          if (!cursor.ReadTimestamp(fr.get_ticks_per_second(), &ts)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          if (ts < 0) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_timestamp_overflow);
            return;
          }

          uint64_t waking_tid;
          if (!cursor.ReadUint64(&waking_tid)) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }
          Thread& waking_thread = GetThread(waking_tid);

          auto maybe_args = FuchsiaTraceParser::ParseArgs(
              cursor, argument_count, intern_string, get_string);
          if (!maybe_args.has_value()) {
            context_->stats_tracker->IncrementStats(
                stats::fuchsia_record_read_error);
            return;
          }

          int32_t waking_weight = 0;
          std::optional<UniqueTid> waker_utid = std::nullopt;
          std::optional<uint64_t> waker_tid = std::nullopt;

          for (const auto& arg : *maybe_args) {
            if (arg.name == weight_id_) {
              if (arg.value.Type() !=
                  fuchsia_trace_utils::ArgValue::ArgType::kInt32) {
                context_->stats_tracker->IncrementStats(
                    stats::fuchsia_invalid_event_arg_type);
                return;
              }
              waking_weight = arg.value.Int32();
            } else if (arg.name == waker_id_) {
              if (arg.value.Type() !=
                  fuchsia_trace_utils::ArgValue::ArgType::kKoid) {
                context_->stats_tracker->IncrementStats(
                    stats::fuchsia_invalid_event_arg_type);
                return;
              }
              waker_tid = arg.value.Koid();
              waker_utid = context_->process_tracker->GetOrCreateThread(
                  static_cast<uint32_t>(*waker_tid));
            }
          }

          const bool waking_is_idle = waking_weight == kIdleWeight;
          if (!waking_is_idle) {
            Wake(&waking_thread, ts, cpu, waker_utid, waker_tid);
          }
          break;
        }
        default:
          PERFETTO_DLOG("Skipping unknown scheduler event type %u", event_type);
          break;
      }
      break;
    }
    default: {
      PERFETTO_DFATAL("Unknown record type %u in FuchsiaTraceParser",
                      record_type);
      break;
    }
  }
}

void FuchsiaTraceParser::SwitchFrom(Thread* thread,
                                    int64_t ts,
                                    uint32_t cpu,
                                    uint32_t thread_state) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();

  StringId state = IdForOutgoingThreadState(thread_state);
  UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(thread->info.tid),
                                       static_cast<uint32_t>(thread->info.pid));

  const auto duration = ts - thread->last_ts;
  thread->last_ts = ts;

  // Close the slice record if one is open for this thread.
  if (thread->last_slice_row.has_value()) {
    auto row_ref = thread->last_slice_row->ToRowReference(
        storage->mutable_sched_slice_table());
    row_ref.set_dur(duration);
    row_ref.set_end_state(state);
    thread->last_slice_row.reset();
  }

  // Close the state record if one is open for this thread.
  if (thread->last_state_row.has_value()) {
    auto row_ref = thread->last_state_row->ToRowReference(
        storage->mutable_thread_state_table());
    row_ref.set_dur(duration);
    thread->last_state_row.reset();
  }

  // Open a new state record to track the duration of the outgoing
  // state.
  tables::ThreadStateTable::Row state_row;
  state_row.ts = ts;
  state_row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  state_row.dur = -1;
  state_row.state = state;
  state_row.utid = utid;
  auto state_row_number =
      storage->mutable_thread_state_table()->Insert(state_row).row_number;
  thread->last_state_row = state_row_number;
}

void FuchsiaTraceParser::SwitchTo(Thread* thread,
                                  int64_t ts,
                                  uint32_t cpu,
                                  int32_t weight) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();

  UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(thread->info.tid),
                                       static_cast<uint32_t>(thread->info.pid));

  const auto duration = ts - thread->last_ts;
  thread->last_ts = ts;

  // Close the state record if one is open for this thread.
  if (thread->last_state_row.has_value()) {
    auto row_ref = thread->last_state_row->ToRowReference(
        storage->mutable_thread_state_table());
    row_ref.set_dur(duration);
    thread->last_state_row.reset();
  }

  auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  // Open a new slice record for this thread.
  tables::SchedSliceTable::Row slice_row;
  slice_row.ts = ts;
  slice_row.ucpu = ucpu;
  slice_row.dur = -1;
  slice_row.utid = utid;
  slice_row.priority = weight;
  auto slice_row_number =
      storage->mutable_sched_slice_table()->Insert(slice_row).row_number;
  thread->last_slice_row = slice_row_number;

  // Open a new state record for this thread.
  tables::ThreadStateTable::Row state_row;
  state_row.ts = ts;
  state_row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  state_row.dur = -1;
  state_row.state = running_string_id_;
  state_row.utid = utid;
  auto state_row_number =
      storage->mutable_thread_state_table()->Insert(state_row).row_number;
  thread->last_state_row = state_row_number;
}

void FuchsiaTraceParser::Wake(Thread* thread,
                              int64_t ts,
                              uint32_t cpu,
                              std::optional<UniqueTid> waker_utid,
                              std::optional<uint64_t> waker_tid) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();

  UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(thread->info.tid),
                                       static_cast<uint32_t>(thread->info.pid));

  const auto duration = ts - thread->last_ts;
  thread->last_ts = ts;

  // Capture waker_id before resetting last_state_row
  std::optional<tables::ThreadStateTable::Id> waker_id = std::nullopt;
  if (waker_tid.has_value()) {
    auto waker_it = threads_.find(*waker_tid);
    if (waker_it != threads_.end() &&
        waker_it->second.last_state_row.has_value()) {
      waker_id = waker_it->second.last_state_row
                     ->ToRowReference(storage->mutable_thread_state_table())
                     .id();
    }
  } else if (thread->last_state_row.has_value()) {
    waker_id = thread->last_state_row
                   ->ToRowReference(storage->mutable_thread_state_table())
                   .id();
  }

  // Close the state record if one is open for this thread.
  if (thread->last_state_row.has_value()) {
    auto row_ref = thread->last_state_row->ToRowReference(
        storage->mutable_thread_state_table());
    row_ref.set_dur(duration);
    thread->last_state_row.reset();
  }

  // Open a new state record for this thread.
  tables::ThreadStateTable::Row state_row;
  state_row.ts = ts;
  state_row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  state_row.dur = -1;
  state_row.state = waking_string_id_;
  state_row.utid = utid;

  // Identify the waker for this thread's wakeup event. If Zircon doesn't
  // report a waker, we infer that the thread woke itself w/ timer/timeout/etc.
  state_row.waker_utid = waker_utid.value_or(utid);
  state_row.waker_id = waker_id;

  auto state_row_number =
      storage->mutable_thread_state_table()->Insert(state_row).row_number;
  thread->last_state_row = state_row_number;
}

void FuchsiaTraceParser::ParseCpuFrequencyCounter(
    int64_t ts,
    const std::vector<Arg>& args) {
  std::optional<uint32_t> cpu;
  std::optional<double> frequency_khz;
  // A record which repeats "cpu" or "value", or which carries an unusable core
  // index or frequency, does not honour the contract and is dropped whole.
  bool is_valid_event = true;
  for (const Arg& arg : args) {
    if (arg.name == cpu_frequency_cpu_arg_id_) {
      is_valid_event = is_valid_event && !cpu.has_value();
      cpu = ExtractCpuCoreIndex(arg.value);
      is_valid_event = is_valid_event && cpu.has_value();
    } else if (arg.name == cpu_frequency_value_arg_id_) {
      is_valid_event = is_valid_event && !frequency_khz.has_value();
      frequency_khz = ExtractFrequencyKhz(arg.value);
      is_valid_event = is_valid_event && frequency_khz.has_value();
    }
  }

  if (!is_valid_event || !cpu.has_value() || !frequency_khz.has_value()) {
    context_->stats_tracker->IncrementStats(stats::fuchsia_invalid_event);
    return;
  }

  context_->cpu_tracker->GetOrCreateCpu(*cpu);
  TrackId track = context_->track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(*cpu));
  context_->event_tracker->PushCounter(ts, *frequency_khz, track);
}

void FuchsiaTraceParser::MaybePushCpuIdleTransition(int64_t ts,
                                                    uint32_t cpu,
                                                    bool outgoing_is_idle,
                                                    bool incoming_is_idle) {
  if (outgoing_is_idle == incoming_is_idle) {
    return;
  }
  context_->cpu_tracker->GetOrCreateCpu(cpu);
  TrackId idle_track = context_->track_tracker->InternTrack(
      tracks::kCpuIdleBlueprint, tracks::Dimensions(cpu));
  double value = incoming_is_idle ? kCpuIdleState : kCpuActiveState;
  context_->event_tracker->PushCounter(ts, value, idle_track);
}

StringId FuchsiaTraceParser::IdForOutgoingThreadState(uint32_t state) {
  switch (state) {
    case kThreadNew:
    case kThreadRunning:
      return runnable_string_id_;
    case kThreadBlocked:
      return blocked_string_id_;
    case kThreadSuspended:
      return suspended_string_id_;
    case kThreadDying:
      return exit_dying_string_id_;
    case kThreadDead:
      return exit_dead_string_id_;
    default:
      return kNullStringId;
  }
}

std::optional<FuchsiaTraceParser::ResolvedFlow>
FuchsiaTraceParser::GetGlobalFlowId(uint32_t pid,
                                    uint64_t correlation_id,
                                    bool step_or_end) {
  FuchsiaScopedFlowId scoped_id = {pid, correlation_id};

  // First, check if there is an exact match for the process-scoped flow.
  auto* it = fuchsia_active_flows_.Find(scoped_id);
  if (it) {
    return ResolvedFlow{*it, scoped_id};
  }

  // If this is a flow begin event, allocate a new globally unique flow ID.
  if (!step_or_end) {
    uint64_t global_id = fuchsia_global_flow_id_counter_++;
    fuchsia_active_flows_.Insert(scoped_id, global_id);

    auto* global_it = fuchsia_global_flows_.Find(correlation_id);
    if (global_it) {
      if (global_it->pid != pid) {
        global_it->is_ambiguous = true;
      }
    } else {
      fuchsia_global_flows_.Insert(correlation_id, GlobalFlowInfo{pid, false});
    }
    return ResolvedFlow{global_id, scoped_id};
  }

  // For step or end events, check if the correlation ID is registered under a
  // different process PID, indicating a cross-process flow.
  auto* global_it = fuchsia_global_flows_.Find(correlation_id);
  if (global_it) {
    if (global_it->is_ambiguous) {
      // The correlation ID is defined in multiple processes, so we cannot
      // disambiguate it for cross-process correlation.
      return std::nullopt;
    }
    uint32_t owner_pid = global_it->pid;
    FuchsiaScopedFlowId owner_scoped_id = {owner_pid, correlation_id};
    auto* global_id_it = fuchsia_active_flows_.Find(owner_scoped_id);
    if (global_id_it) {
      return ResolvedFlow{*global_id_it, owner_scoped_id};
    }
  }

  return std::nullopt;
}

}  // namespace perfetto::trace_processor
