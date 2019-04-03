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

#include "src/trace_processor/json_trace_tokenizer.h"

#include <json/reader.h>
#include <json/value.h>

#include "src/trace_processor/json_trace_utils.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"

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
  int square_brackets = 0;
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

    // TODO(primiano): skip braces in quoted strings, e.g.: {"foo": "ba{z" }
  }
  return kNeedsMoreData;
}

}  // namespace

JsonTraceTokenizer::JsonTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx) {}
JsonTraceTokenizer::~JsonTraceTokenizer() = default;

bool JsonTraceTokenizer::Parse(std::unique_ptr<uint8_t[]> data, size_t size) {
  buffer_.insert(buffer_.end(), data.get(), data.get() + size);
  const char* buf = buffer_.data();
  const char* next = buf;
  const char* end = buf + buffer_.size();

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

  auto* trace_sorter = context_->sorter.get();

  while (next < end) {
    std::unique_ptr<Json::Value> value(new Json::Value());
    const auto res = ReadOneJsonDict(next, end, value.get(), &next);
    if (res == kFatalError)
      return false;
    if (res == kEndOfTrace || res == kNeedsMoreData)
      break;

    base::Optional<int64_t> opt_ts =
        json_trace_utils::CoerceToNs((*value)["ts"]);
    PERFETTO_CHECK(opt_ts.has_value());
    int64_t ts = opt_ts.value();

    trace_sorter->PushJsonValue(ts, std::move(value));
  }

  offset_ += static_cast<uint64_t>(next - buf);
  buffer_.erase(buffer_.begin(), buffer_.begin() + (next - buf));
  return true;
}

}  // namespace trace_processor
}  // namespace perfetto
