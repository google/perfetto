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

#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"

namespace perfetto {
namespace trace_processor {

namespace {
// Record Types
const uint32_t kEvent = 4;

// Event Types
const uint32_t kDurationBegin = 2;
const uint32_t kDurationEnd = 3;
const uint32_t kDurationComplete = 4;
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

      // Skip over all the args, so that the |ReadTimestamp| call for complete
      // durations has the pointer in the right place.
      for (uint32_t i = 0; i < n_args; i++) {
        uint64_t arg_header = *current;
        uint32_t arg_size_words =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 4, 15);
        current += arg_size_words;
      }

      switch (event_type) {
        case kDurationBegin: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          slices->Begin(ts, utid, cat, name);
          break;
        }
        case kDurationEnd: {
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          slices->End(ts, utid, cat, name);
          break;
        }
        case kDurationComplete: {
          int64_t end_ts = fuchsia_trace_utils::ReadTimestamp(
              &current, provider_view->get_ticks_per_second());
          UniqueTid utid =
              procs->UpdateThread(static_cast<uint32_t>(tinfo.tid),
                                  static_cast<uint32_t>(tinfo.pid));
          slices->Scoped(ts, utid, cat, name, end_ts - ts);
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
