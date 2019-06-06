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

#include "src/trace_processor/systrace_parser.h"

#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"

namespace perfetto {
namespace trace_processor {

SystraceParser::SystraceParser(TraceProcessorContext* ctx)
    : context_(ctx), lmk_id_(ctx->storage->InternString("mem.lmk")) {}

void SystraceParser::ParsePrintEvent(int64_t ts,
                                     uint32_t pid,
                                     base::StringView event) {
  systrace_utils::SystraceTracePoint point{};
  switch (ParseSystraceTracePoint(event, &point)) {
    case systrace_utils::SystraceParseResult::kSuccess:
      ParseSystracePoint(ts, pid, point);
      break;
    case systrace_utils::SystraceParseResult::kFailure:
      context_->storage->IncrementStats(stats::systrace_parse_failure);
      break;
    case systrace_utils::SystraceParseResult::kUnsupported:
      // Silently ignore unsupported results.
      break;
  }
}

void SystraceParser::ParseZeroEvent(int64_t ts,
                                    uint32_t pid,
                                    int32_t flag,
                                    base::StringView name,
                                    uint32_t tgid,
                                    int64_t value) {
  systrace_utils::SystraceTracePoint point{};
  point.name = name;
  point.tgid = tgid;
  point.value = value;

  // The value of these constants can be found in the msm-google kernel.
  constexpr int32_t kSystraceEventBegin = 1 << 0;
  constexpr int32_t kSystraceEventEnd = 1 << 1;
  constexpr int32_t kSystraceEventInt64 = 1 << 2;

  if ((flag & kSystraceEventBegin) != 0) {
    point.phase = 'B';
  } else if ((flag & kSystraceEventEnd) != 0) {
    point.phase = 'E';
  } else if ((flag & kSystraceEventInt64) != 0) {
    point.phase = 'C';
  } else {
    context_->storage->IncrementStats(stats::systrace_parse_failure);
    return;
  }
  context_->systrace_parser->ParseSystracePoint(ts, pid, point);
}

void SystraceParser::ParseSystracePoint(
    int64_t ts,
    uint32_t pid,
    systrace_utils::SystraceTracePoint point) {
  switch (point.phase) {
    case 'B': {
      StringId name_id = context_->storage->InternString(point.name);
      context_->slice_tracker->BeginAndroid(ts, pid, point.tgid, 0 /*cat_id*/,
                                            name_id);
      break;
    }

    case 'E': {
      context_->slice_tracker->EndAndroid(ts, pid, point.tgid);
      break;
    }

    case 'C': {
      // LMK events from userspace are hacked as counter events with the "value"
      // of the counter representing the pid of the killed process which is
      // reset to 0 once the kill is complete.
      // Homogenise this with kernel LMK events as an instant event, ignoring
      // the resets to 0.
      if (point.name == "kill_one_process") {
        auto killed_pid = static_cast<uint32_t>(point.value);
        if (killed_pid != 0) {
          UniquePid killed_upid =
              context_->process_tracker->GetOrCreateProcess(killed_pid);
          context_->event_tracker->PushInstant(ts, lmk_id_, 0, killed_upid,
                                               RefType::kRefUpid);
        }
        // TODO(lalitm): we should not add LMK events to the counters table
        // once the UI has support for displaying instants.
      }
      // This is per upid on purpose. Some counters are pushed from arbitrary
      // threads but are really per process.
      UniquePid upid =
          context_->process_tracker->GetOrCreateProcess(point.tgid);
      StringId name_id = context_->storage->InternString(point.name);
      context_->event_tracker->PushCounter(ts, point.value, name_id, upid,
                                           RefType::kRefUpid);
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
