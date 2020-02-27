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

// For bazel build.
#include "perfetto/base/build_config.h"
#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)

#include "src/trace_processor/importers/json/json_trace_tokenizer.h"

#include <json/reader.h>
#include <json/value.h>

#include "src/trace_processor/importers/json/json_trace_utils.h"
#include "src/trace_processor/importers/json/json_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"

namespace perfetto {
namespace trace_processor {

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
  int square_brackets = 0;
  const char* dict_begin = nullptr;
  bool in_string = false;
  bool is_escaping = false;
  for (const char* s = start; s < end; s++) {
    if (isspace(*s) || *s == ',')
      continue;
    if (*s == '"' && !is_escaping) {
      in_string = !in_string;
      continue;
    }
    if (in_string) {
      // If we're in a string and we see a backslash and the last character was
      // not a backslash the next character is escaped:
      is_escaping = *s == '\\' && !is_escaping;
      // If we're currently parsing a string we should ignore otherwise special
      // characters:
      continue;
    }
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
    if (*s == '[') {
      square_brackets++;
      continue;
    }
    if (*s == ']') {
      if (square_brackets == 0) {
        // We've reached the end of [traceEvents] array.
        // There might be other top level keys in the json (e.g. metadata)
        // after.
        // TODO(dproy): Handle trace metadata importing.
        return kEndOfTrace;
      }
      square_brackets--;
    }
  }
  return kNeedsMoreData;
}

JsonTraceTokenizer::JsonTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}
JsonTraceTokenizer::~JsonTraceTokenizer() = default;

util::Status JsonTraceTokenizer::Parse(std::unique_ptr<uint8_t[]> data,
                                       size_t size) {
  buffer_.insert(buffer_.end(), data.get(), data.get() + size);
  const char* buf = buffer_.data();
  const char* next = buf;
  const char* end = buf + buffer_.size();

  JsonTracker* json_tracker = JsonTracker::GetOrCreate(context_);

  // It's possible the displayTimeUnit key is at the end of the json
  // file so to be correct we ought to parse the whole file looking
  // for this key before parsing any events however this would require
  // two passes on the file so for now we only handle displayTimeUnit
  // correctly if it is at the beginning of the file.
  const base::StringView view(buf, size);
  if (view.find("\"displayTimeUnit\":\"ns\"") != base::StringView::npos) {
    json_tracker->SetTimeUnit(json::TimeUnit::kNs);
  } else if (view.find("\"displayTimeUnit\":\"ms\"") !=
             base::StringView::npos) {
    json_tracker->SetTimeUnit(json::TimeUnit::kMs);
  }

  if (offset_ == 0) {
    // Trace could begin in any of these ways:
    // {"traceEvents":[{
    // { "traceEvents": [{
    // [{
    // Skip up to the first '['
    while (next != end && *next != '[') {
      next++;
    }
    if (next == end)
      return util::ErrStatus("Failed to parse: first chunk missing opening [");
    next++;
  }

  auto* trace_sorter = context_->sorter.get();

  while (next < end) {
    std::unique_ptr<Json::Value> value(new Json::Value());
    const auto res = ReadOneJsonDict(next, end, value.get(), &next);
    if (res == kFatalError)
      return util::ErrStatus("Encountered fatal error while parsing JSON");
    if (res == kEndOfTrace || res == kNeedsMoreData)
      break;

    base::Optional<int64_t> opt_ts = json_tracker->CoerceToTs((*value)["ts"]);
    int64_t ts = 0;
    if (opt_ts.has_value()) {
      ts = opt_ts.value();
    } else {
      // Metadata events may omit ts. In all other cases error:
      auto& ph = (*value)["ph"];
      if (!ph.isString() || *ph.asCString() != 'M') {
        context_->storage->IncrementStats(stats::json_tokenizer_failure);
        continue;
      }
    }
    trace_sorter->PushJsonValue(ts, std::move(value));
  }

  offset_ += static_cast<uint64_t>(next - buf);
  buffer_.erase(buffer_.begin(), buffer_.begin() + (next - buf));
  return util::OkStatus();
}

void JsonTraceTokenizer::NotifyEndOfFile() {}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
