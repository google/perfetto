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

#include "src/trace_processor/json_trace_parser.h"

#include <inttypes.h>
#include <json/reader.h>
#include <json/value.h>

#include <limits>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/json_trace_utils.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#error The JSON trace parser is supported only in the standalone build for now.
#endif

namespace perfetto {
namespace trace_processor {

JsonTraceParser::JsonTraceParser(TraceProcessorContext* context)
    : context_(context) {}

JsonTraceParser::~JsonTraceParser() = default;

void JsonTraceParser::ParseFtracePacket(uint32_t,
                                        int64_t,
                                        TraceSorter::TimestampedTracePiece) {
  PERFETTO_FATAL("Json Trace Parser cannot handle ftrace packets.");
}

void JsonTraceParser::ParseTracePacket(int64_t timestamp,
                                       TraceSorter::TimestampedTracePiece ttp) {
  PERFETTO_DCHECK(ttp.json_value != nullptr);
  const Json::Value& value = *(ttp.json_value);

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  SliceTracker* slice_tracker = context_->slice_tracker.get();

  auto& ph = value["ph"];
  if (!ph.isString())
    return;
  char phase = *ph.asCString();

  base::Optional<uint32_t> opt_pid;
  base::Optional<uint32_t> opt_tid;

  if (value.isMember("pid"))
    opt_pid = json_trace_utils::CoerceToUint32(value["pid"]);
  if (value.isMember("tid"))
    opt_tid = json_trace_utils::CoerceToUint32(value["tid"]);

  uint32_t pid = opt_pid.value_or(0);
  uint32_t tid = opt_tid.value_or(pid);

  base::StringView cat = value.isMember("cat")
                             ? base::StringView(value["cat"].asCString())
                             : base::StringView();
  base::StringView name = value.isMember("name")
                              ? base::StringView(value["name"].asCString())
                              : base::StringView();

  StringId cat_id = storage->InternString(cat);
  StringId name_id = storage->InternString(name);
  UniqueTid utid = procs->UpdateThread(tid, pid);

  switch (phase) {
    case 'B': {  // TRACE_EVENT_BEGIN.
      slice_tracker->Begin(timestamp, utid, cat_id, name_id);
      break;
    }
    case 'E': {  // TRACE_EVENT_END.
      slice_tracker->End(timestamp, utid, cat_id, name_id);
      break;
    }
    case 'X': {  // TRACE_EVENT (scoped event).
      base::Optional<int64_t> opt_dur =
          json_trace_utils::CoerceToNs(value["dur"]);
      if (!opt_dur.has_value())
        return;
      slice_tracker->Scoped(timestamp, utid, cat_id, name_id, opt_dur.value());
      break;
    }
    case 'M': {  // Metadata events (process and thread names).
      if (strcmp(value["name"].asCString(), "thread_name") == 0) {
        const char* thread_name = value["args"]["name"].asCString();
        auto thread_name_id = context_->storage->InternString(thread_name);
        procs->UpdateThreadName(tid, thread_name_id);
        break;
      }
      if (strcmp(value["name"].asCString(), "process_name") == 0) {
        const char* proc_name = value["args"]["name"].asCString();
        procs->UpdateProcess(pid, base::nullopt, proc_name);
        break;
      }
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
