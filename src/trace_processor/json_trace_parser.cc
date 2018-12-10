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
#include "perfetto/base/utils.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) || \
    PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
#error The JSON trace parser is supported only in the standalone build for now.
#endif

namespace perfetto {
namespace trace_processor {
namespace {

enum ReadDictRes { kFoundDict, kNeedsMoreData, kEndOfTrace, kFatalError };

// Parses at most one JSON dictionary and returns a pointer to the end of it,
// or nullptr if no dict could be detected.
// This is to avoid decoding the full trace in memory and reduce heap traffic.
// E.g.  input:  { a:1 b:{ c:2, d:{ e:3 } } } , { a:4, ... },
//       output: [   only this is parsed    ] ^return value points here.
ReadDictRes ReadOneJsonDict(const char* start,
                            const char* end,
                            Json::Value* value,
                            const char** next) {
  int braces = 0;
  const char* dict_begin = nullptr;
  for (const char* s = start; s < end; s++) {
    if (isspace(*s) || *s == ',')
      continue;
    if (*s == '{') {
      if (braces == 0)
        dict_begin = s;
      braces++;
      continue;
    }
    if (*s == '}') {
      if (braces <= 0)
        return kEndOfTrace;
      if (--braces > 0)
        continue;
      Json::Reader reader;
      if (!reader.parse(dict_begin, s + 1, *value, /*collectComments=*/false)) {
        PERFETTO_ELOG("JSON error: %s",
                      reader.getFormattedErrorMessages().c_str());
        return kFatalError;
      }
      *next = s + 1;
      return kFoundDict;
    }
    // TODO(primiano): skip braces in quoted strings, e.g.: {"foo": "ba{z" }
  }
  return kNeedsMoreData;
}

}  // namespace

bool CoerceToInt64(const Json::Value& value, int64_t* integer_ptr) {
  switch (static_cast<size_t>(value.type())) {
    case Json::uintValue:
    case Json::intValue:
      *integer_ptr = value.asInt64();
      return true;
    case Json::stringValue: {
      std::string s = value.asString();
      char* end;
      *integer_ptr = strtoll(s.c_str(), &end, 10);
      return end == s.data() + s.size();
    }
    default:
      return false;
  }
}

bool CoerceToUint32(const Json::Value& value, uint32_t* integer_ptr) {
  int64_t n = 0;
  if (!CoerceToInt64(value, &n))
    return false;
  if (n < 0 || n > std::numeric_limits<uint32_t>::max())
    return false;
  *integer_ptr = static_cast<uint32_t>(n);
  return true;
}

JsonTraceParser::JsonTraceParser(TraceProcessorContext* context)
    : context_(context) {}

JsonTraceParser::~JsonTraceParser() = default;

bool JsonTraceParser::Parse(std::unique_ptr<uint8_t[]> data, size_t size) {
  buffer_.insert(buffer_.end(), data.get(), data.get() + size);
  char* buf = &buffer_[0];
  const char* next = buf;
  const char* end = &buffer_[buffer_.size()];

  if (offset_ == 0) {
    // Trace could begin in any of these ways:
    // {"traceEvents":[{
    // { "traceEvents": [{
    // [{
    // Skip up to the first '['
    while (next != end && *next != '[') {
      next++;
    }
    if (next == end) {
      PERFETTO_ELOG("Failed to parse: first chunk missing opening [");
      return false;
    }
    next++;
  }

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  SliceTracker* slice_tracker = context_->slice_tracker.get();

  while (next < end) {
    Json::Value value;
    const auto res = ReadOneJsonDict(next, end, &value, &next);
    if (res == kFatalError)
      return false;
    if (res == kEndOfTrace || res == kNeedsMoreData)
      break;
    auto& ph = value["ph"];
    if (!ph.isString())
      continue;
    char phase = *ph.asCString();
    uint32_t tid = 0;
    PERFETTO_CHECK(CoerceToUint32(value["tid"], &tid));
    uint32_t pid = 0;
    PERFETTO_CHECK(CoerceToUint32(value["pid"], &pid));
    int64_t ts = 0;
    PERFETTO_CHECK(CoerceToInt64(value["ts"], &ts));
    ts *= 1000;
    const char* cat = value.isMember("cat") ? value["cat"].asCString() : "";
    const char* name = value.isMember("name") ? value["name"].asCString() : "";
    StringId cat_id = storage->InternString(cat);
    StringId name_id = storage->InternString(name);
    UniqueTid utid = procs->UpdateThread(tid, pid);

    switch (phase) {
      case 'B': {  // TRACE_EVENT_BEGIN.
        slice_tracker->Begin(ts, utid, cat_id, name_id);
        break;
      }
      case 'E': {  // TRACE_EVENT_END.
        slice_tracker->End(ts, utid, cat_id, name_id);
        break;
      }
      case 'X': {  // TRACE_EVENT (scoped event).
        int64_t duration = 0;
        // Ignore this event if invalid.
        if (!CoerceToInt64(value["dur"], &duration))
          continue;
        duration *= 1000;
        slice_tracker->Scoped(ts, utid, cat_id, name_id, duration);
        break;
      }
      case 'M': {  // Metadata events (process and thread names).
        if (strcmp(value["name"].asCString(), "thread_name") == 0) {
          const char* thread_name = value["args"]["name"].asCString();
          procs->UpdateThreadName(tid, pid, thread_name);
          break;
        }
        if (strcmp(value["name"].asCString(), "process_name") == 0) {
          const char* proc_name = value["args"]["name"].asCString();
          procs->UpdateProcess(pid, proc_name);
          break;
        }
      }
    }
  }
  offset_ += static_cast<uint64_t>(next - buf);
  buffer_.erase(buffer_.begin(), buffer_.begin() + (next - buf));
  return true;
}

}  // namespace trace_processor
}  // namespace perfetto
