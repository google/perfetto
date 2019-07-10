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

#include "src/trace_processor/fuchsia_trace_parser.h"

#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"

namespace perfetto {
namespace trace_processor {

namespace {
// Record Types
constexpr uint32_t kEvent = 4;

// Event Types
constexpr uint32_t kInstant = 0;
constexpr uint32_t kCounter = 1;
constexpr uint32_t kDurationBegin = 2;
constexpr uint32_t kDurationEnd = 3;
constexpr uint32_t kDurationComplete = 4;
constexpr uint32_t kAsyncBegin = 5;
constexpr uint32_t kAsyncInstant = 6;
constexpr uint32_t kAsyncEnd = 7;

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

struct Arg {
  StringId name;
  fuchsia_trace_utils::ArgValue value;
};
}  // namespace

FuchsiaTraceParser::FuchsiaTraceParser(TraceProcessorContext* context)
    : context_(context) {}

FuchsiaTraceParser::~FuchsiaTraceParser() = default;

void FuchsiaTraceParser::ParseFtracePacket(uint32_t,
                                           int64_t,
                                           TraceSorter::TimestampedTracePiece) {
  PERFETTO_FATAL("Fuchsia Trace Parser cannot handle ftrace packets.");
}

void FuchsiaTraceParser::ParseTracePacket(
    int64_t,
    TraceSorter::TimestampedTracePiece ttp) {
  PERFETTO_DCHECK(ttp.fuchsia_provider_view != nullptr);

  // The timestamp is also present in the record, so we'll ignore the one passed
  // as an argument.
  const uint64_t* current =
      reinterpret_cast<const uint64_t*>(ttp.blob_view.data());
  FuchsiaProviderView* provider_view = ttp.fuchsia_provider_view.get();
  ProcessTracker* procs = context_->process_tracker.get();
  SliceTracker* slices = context_->slice_tracker.get();

  uint64_t header = *current++;
  uint32_t record_type = fuchsia_trace_utils::ReadField<uint32_t>(header, 0, 3);
  switch (record_type) {
    case kEvent: {
      uint32_t event_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
      uint32_t n_args =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 23);
      uint32_t thread_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 31);
      uint32_t cat_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 32, 47);
      uint32_t name_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 48, 63);

      int64_t ts = fuchsia_trace_utils::ReadTimestamp(
          &current, provider_view->get_ticks_per_second());
      fuchsia_trace_utils::ThreadInfo tinfo;
      if (fuchsia_trace_utils::IsInlineThread(thread_ref)) {
        tinfo = fuchsia_trace_utils::ReadInlineThread(&current);
      } else {
        tinfo = provider_view->GetThread(thread_ref);
      }
      StringId cat;
      if (fuchsia_trace_utils::IsInlineString(cat_ref)) {
        cat = context_->storage->InternString(
            fuchsia_trace_utils::ReadInlineString(&current, cat_ref));
      } else {
        cat = provider_view->GetString(cat_ref);
      }
      StringId name;
      if (fuchsia_trace_utils::IsInlineString(name_ref)) {
        name = context_->storage->InternString(
            fuchsia_trace_utils::ReadInlineString(&current, name_ref));
      } else {
        name = provider_view->GetString(name_ref);
      }

      // Read arguments
      std::vector<Arg> args;
      for (uint32_t i = 0; i < n_args; i++) {
        const uint64_t* arg_base = current;
        uint64_t arg_header = *current++;
        uint32_t arg_type =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 0, 3);
        uint32_t arg_size_words =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 4, 15);
        uint32_t arg_name_ref =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 16, 31);
        Arg arg;
        if (fuchsia_trace_utils::IsInlineString(arg_name_ref)) {
          arg.name = context_->storage->InternString(
              fuchsia_trace_utils::ReadInlineString(&current, arg_name_ref));
        } else {
          arg.name = provider_view->GetString(arg_name_ref);
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
          case kInt64:
            arg.value = fuchsia_trace_utils::ArgValue::Int64(
                static_cast<int64_t>(*current++));
            break;
          case kUint64:
            arg.value = fuchsia_trace_utils::ArgValue::Uint64(*current++);
            break;
          case kDouble: {
            double value;
            memcpy(&value, current, sizeof(double));
            current++;
            arg.value = fuchsia_trace_utils::ArgValue::Double(value);
            break;
          }
          case kString: {
            uint32_t arg_value_ref =
                fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 32, 47);
            StringId value;
            if (fuchsia_trace_utils::IsInlineString(arg_value_ref)) {
              value = context_->storage->InternString(
                  fuchsia_trace_utils::ReadInlineString(&current,
                                                        arg_value_ref));
            } else {
              value = provider_view->GetString(arg_value_ref);
            }
            arg.value = fuchsia_trace_utils::ArgValue::String(value);
            break;
          }
          case kPointer:
            arg.value = fuchsia_trace_utils::ArgValue::Pointer(*current++);
            break;
          case kKoid:
            arg.value = fuchsia_trace_utils::ArgValue::Koid(*current++);
            break;
          default:
            arg.value = fuchsia_trace_utils::ArgValue::Unknown();
            break;
        }

        args.push_back(arg);
        current = arg_base + arg_size_words;
      }

      switch (event_type) {
        case kInstant: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          RowId row = context_->event_tracker->PushInstant(ts, name, 0, utid,
                                                           RefType::kRefUtid);
          for (const Arg& arg : args) {
            context_->args_tracker->AddArg(
                row, arg.name, arg.name,
                arg.value.ToStorageVariadic(context_->storage.get()));
          }
          context_->args_tracker->Flush();
          break;
        }
        case kCounter: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          std::string name_str =
              context_->storage->GetString(name).ToStdString();
          // Note: In the Fuchsia trace format, counter values are stored in the
          // arguments for the record, with the data series defined by both the
          // record name and the argument name. In Perfetto, counters only have
          // one name, so we combine both names into one here.
          for (const Arg& arg : args) {
            std::string counter_name_str = name_str + ":";
            counter_name_str += context_->storage->GetString(arg.name).c_str();
            bool is_valid_value = false;
            double counter_value = -1;
            switch (arg.value.Type()) {
              case fuchsia_trace_utils::ArgValue::kInt32:
                is_valid_value = true;
                counter_value = static_cast<double>(arg.value.Int32());
                break;
              case fuchsia_trace_utils::ArgValue::kUint32:
                is_valid_value = true;
                counter_value = static_cast<double>(arg.value.Uint32());
                break;
              case fuchsia_trace_utils::ArgValue::kInt64:
                is_valid_value = true;
                counter_value = static_cast<double>(arg.value.Int64());
                break;
              case fuchsia_trace_utils::ArgValue::kUint64:
                is_valid_value = true;
                counter_value = static_cast<double>(arg.value.Uint64());
                break;
              case fuchsia_trace_utils::ArgValue::kDouble:
                is_valid_value = true;
                counter_value = arg.value.Double();
                break;
              case fuchsia_trace_utils::ArgValue::kNull:
              case fuchsia_trace_utils::ArgValue::kString:
              case fuchsia_trace_utils::ArgValue::kPointer:
              case fuchsia_trace_utils::ArgValue::kKoid:
              case fuchsia_trace_utils::ArgValue::kUnknown:
                context_->storage->IncrementStats(
                    stats::fuchsia_non_numeric_counters);
                break;
            }
            if (is_valid_value) {
              context_->event_tracker->PushCounter(
                  ts, counter_value,
                  context_->storage->InternString(
                      base::StringView(counter_name_str)),
                  utid, kRefUtid);
            }
          }
          break;
        }
        case kDurationBegin: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          slices->Begin(ts, utid, RefType::kRefUtid, cat, name);
          break;
        }
        case kDurationEnd: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          // TODO(b/131181693): |cat| and |name| are not passed here so that
          // if two slices end at the same timestep, the slices get closed in
          // the correct order regardless of which end event is processed first.
          slices->End(ts, utid, RefType::kRefUtid);
          break;
        }
        case kDurationComplete: {
          int64_t end_ts = fuchsia_trace_utils::ReadTimestamp(
              &current, provider_view->get_ticks_per_second());
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          slices->Scoped(ts, utid, RefType::kRefUtid, cat, name, end_ts - ts);
          break;
        }
        case kAsyncBegin: {
          int64_t correlation_id = static_cast<int64_t>(*current++);
          slices->Begin(ts, correlation_id, RefType::kRefGlobalAsyncTrack, cat,
                        name);
          break;
        }
        case kAsyncInstant: {
          int64_t correlation_id = static_cast<int64_t>(*current++);
          RowId row = context_->event_tracker->PushInstant(
              ts, name, 0, correlation_id, RefType::kRefGlobalAsyncTrack);
          for (const Arg& arg : args) {
            context_->args_tracker->AddArg(
                row, arg.name, arg.name,
                arg.value.ToStorageVariadic(context_->storage.get()));
          }
          context_->args_tracker->Flush();
          break;
        }
        case kAsyncEnd: {
          int64_t correlation_id = static_cast<int64_t>(*current++);
          slices->End(ts, correlation_id, RefType::kRefGlobalAsyncTrack, cat,
                      name);
          break;
        }
      }
      break;
    }
    default: {
      PERFETTO_DFATAL("Unknown record type %d in FuchsiaTraceParser",
                      record_type);
      break;
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
